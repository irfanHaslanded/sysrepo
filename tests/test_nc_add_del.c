/**
 * @author Irfan <irfan@graphiant.com>
 *
 * @copyright
 * Copyright 2022 Graphiant
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define _GNU_SOURCE

#include <string.h>
#include <unistd.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <cmocka.h>
#include <libyang/libyang.h>

#include "sysrepo.h"
#include "common.h"
#include "tests/tcommon.h"

static int test_full_scale = 0;

struct state {
    sr_conn_ctx_t *conn;
    sr_session_ctx_t *sess;
    ATOMIC_T running;
    pthread_barrier_t barrier;
    ATOMIC_T invocation_count;
    uint32_t id;
};

static int
setup_f(void **state)
{
    struct state *st;
    const char *schema_paths[] = {
        TESTS_SRC_DIR "/files/test.yang",
        TESTS_SRC_DIR "/files/ietf-interfaces.yang",
        TESTS_SRC_DIR "/files/ietf-ip.yang",
        TESTS_SRC_DIR "/files/iana-if-type.yang",
        TESTS_SRC_DIR "/files/ietf-if-aug.yang",
        TESTS_SRC_DIR "/files/test-leafref.yang",
        NULL
    };

    st = calloc(1, sizeof *st);
    *state = st;

    if (sr_connect(0, &(st->conn)) != SR_ERR_OK) {
        return 1;
    }

    if (sr_install_modules(st->conn, schema_paths, TESTS_SRC_DIR "/files", NULL) != SR_ERR_OK) {
        return 1;
    }
    return 0;
}

static int
teardown_f(void **state)
{
    struct state *st = (struct state *)*state;
    const char *module_names[] = {
        "ietf-if-aug",
        "iana-if-type",
        "ietf-ip",
        "ietf-interfaces",
        "test",
        "test-leafref",
        NULL
    };

    sr_remove_modules(st->conn, module_names, 0);

    sr_disconnect(st->conn);
    free(st);
    return 0;
}

typedef struct {
    ATOMIC_T count;
} cb_stats;

static int
module_change_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t request_id, void *private_data)
{
    (void)session;
    (void)sub_id;
    (void)module_name;
    (void)xpath;
    (void)event;
    (void)request_id;
    cb_stats *stats = (cb_stats *)private_data;
    ATOMIC_INC_RELAXED(stats->count);
    return 0;
}

static void test_leafref_edit_nc_del(const char *xpath1, const char *xpath2)
{
    int ret = 0;

    struct lyd_node *edit = NULL, *node = NULL;
    sr_data_t *del_root = NULL, *data = NULL;

    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *sess = NULL;
    ret = sr_connect(0, &conn);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_session_start(conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);
    /* Step 1.a Get data */
    ret = sr_get_data(sess, xpath1, 1, 0, 0, &del_root);
    assert_int_equal(ret, SR_ERR_OK);

    /* Step 1.b Find the node to be deleted in this */
    lyd_find_path(del_root->tree, xpath1, 0, &node);

    /* Step 1.c. Add delete attr */
    ret = lyd_new_meta(LYD_CTX(node), node, NULL, "ietf-netconf:operation", "remove", 0, NULL);
    assert_int_equal(ret, SR_ERR_OK);

    lyd_dup_siblings(del_root->tree, NULL, 0, &edit);
    sr_release_data(del_root);
    del_root = NULL;
    /* Step 2.a Get data */
    ret = sr_get_data(sess, xpath2, 1, 0, 0, &del_root);
    assert_int_equal(ret, SR_ERR_OK);
    /* Step 2.b Find the node to be deleted in this */
    ret = lyd_find_path(del_root->tree, xpath2, 0, &node);
    assert_int_equal(ret, SR_ERR_OK);

    /* Step 2.c Add delete attr */
    ret = lyd_new_meta(LYD_CTX(node), node, NULL, "ietf-netconf:operation", "remove", 0, NULL);
    assert_int_equal(ret, SR_ERR_OK);

    /* Step 3. Add new del_root as sibling of edit */
    ret = lyd_insert_sibling(edit, del_root->tree, NULL);
    assert_int_equal(ret, SR_ERR_OK);
    /* Step 4. Edit batch */
    TLOG_INF("Edit batch start");
    ret = sr_edit_batch(sess, edit, "merge");
    assert_int_equal(ret, SR_ERR_OK);

    TLOG_INF("Apply changes start");
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);
    TLOG_INF("Apply changes done");
    lyd_free_all(edit);

    node = NULL;
    TLOG_INF("Checking xpath1");
    ret = sr_get_data(sess, xpath1, 1, 0, 0, &data);
    assert_int_equal(ret, SR_ERR_OK);
    assert_null(data);

    TLOG_INF("Checking xpath2");
    ret = sr_get_data(sess, xpath2, 1, 0, 0, &data);
    assert_int_equal(ret, SR_ERR_OK);
    assert_null(data);

    sr_disconnect(conn);
}

void
test_leafref_edit_with_sub(void **arg)
{
    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *sess = NULL;
    struct state *st = (struct state *)*arg;
    sr_subscription_ctx_t *subscr = NULL;
    int ret;

    cb_stats stats = {0};
    const char *xpath1 = "/test-leafref:car/sensors[name='sensor1']/params[name='temperature']";
    const char *xpath2 = "/test-leafref:car/parameters[name='temperature']";

    ret = sr_connect(0, &conn);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_session_start(conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &st->sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(sess, xpath1, NULL, NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(sess, xpath2, NULL, NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_module_change_subscribe(st->sess, "test-leafref", NULL, module_change_cb, &stats,
                  0, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    test_leafref_edit_nc_del(xpath1, xpath2);

    ret = sr_unsubscribe(subscr);
    assert_int_equal(ret, SR_ERR_OK);
    sr_disconnect(conn);
}

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_leafref_edit_with_sub),
   };
    test_full_scale = getenv("TEST_FULL_SCALE") != NULL;
    setenv("CMOCKA_TEST_ABORT", "1", 1);
    test_log_init();
    return cmocka_run_group_tests(tests, setup_f, teardown_f);
}
