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


struct state {
    sr_conn_ctx_t *conn;
    sr_session_ctx_t *sess;
    pthread_barrier_t barrier;
};

static int test_full_scale;

static int
setup_f(void **state)
{
    struct state *st;
    const char *schema_paths[] = {
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
        "test-leafref",
        NULL
    };

    sr_remove_modules(st->conn, module_names, 0);

    sr_disconnect(st->conn);
    free(st);
    return 0;
}

static void
test_leafref_oper_data(void **arg)
{
    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *oper_sess = NULL;
    struct state *st = (struct state *)*arg;
    int ret;
    sr_data_t *data = NULL;

    const char *a1 = "/test-leafref:car-state/a1";

    const char *xpath =  "/test-leafref:car-state/enabled-params[.='pressure']";
    const char *xpath_del = "/test-leafref:car-state/enabled-params[.='pressure']";

    const char *val = "1 atm";

    ret = sr_connect(0, &conn);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_session_start(st->conn, SR_DS_OPERATIONAL, &oper_sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(oper_sess, a1, "", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_delete_item(oper_sess, xpath_del, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(oper_sess, xpath, val, NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_delete_item(oper_sess, xpath_del, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(oper_sess, xpath, val, NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(oper_sess, 0);
    assert_int_equal(ret, SR_ERR_OK);
    TLOG_INF("Added oper data");

    ret = sr_delete_item(oper_sess, xpath_del, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(oper_sess, 0);
    assert_int_equal(ret, SR_ERR_OK);
    TLOG_INF("Removed oper data");

    ret = sr_get_data(oper_sess, "/test-leafref:*", 0, 0, 0, &data);

    /* Without a proper duplication check in edit_diff.c, this will fail */
    assert_int_equal(ret, SR_ERR_OK);
    sr_release_data(data);
    data = NULL;

    ret = sr_session_stop(oper_sess);
    assert_int_equal(ret, SR_ERR_OK);

    sr_disconnect(conn);
}

static int
dummy_oper_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *module_name, const char *xpath,
        const char *request_xpath, uint32_t request_id, struct lyd_node **parent, void *private_data)
{
    (void)session;
    (void)sub_id;
    (void)module_name;
    (void)xpath;
    (void)request_xpath;
    (void)request_id;
    (void)parent;
    (void)private_data;
    return SR_ERR_OK;
}

/*
 * parallel reading of sysrepo-monitoring data, and oper subscr add/del tests conn ext remap lock.
 * Fetching sysrepo-monitoring in parallel tests 'Node already exists. duplicate issue'
 */
static void*
test_oper_srmon_helper(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *oper_sess = NULL;
    int i, ret;
    sr_data_t *data = NULL;

    int NUM_ITERS = test_full_scale ? 1000 : 10;
    sr_subscription_ctx_t *subscr = NULL;
    char xpath[128];

    static ATOMIC_T id;

    ret = sr_session_start(st->conn, SR_DS_OPERATIONAL, &oper_sess);
    assert_int_equal(ret, SR_ERR_OK);
    pthread_barrier_wait(&st->barrier);

    snprintf(xpath, sizeof xpath, "/test-leafref:car-state/enabled-params[.='key%lu']", ATOMIC_INC_RELAXED(id));

    for (i = 0; i < NUM_ITERS; i++) {
        ret = sr_oper_get_subscribe(oper_sess, "test-leafref", xpath, dummy_oper_cb, NULL, 0, &subscr);
        assert_int_equal(ret, SR_ERR_OK);

        /* Fetch entire operational datastore incl sysrepo-monitoring */
        ret = sr_get_data(oper_sess, "//.", 0, 0, 0, &data);
        assert_int_equal(ret, SR_ERR_OK);
        sr_release_data(data);
        data = NULL;
        ret = sr_unsubscribe(subscr);
        assert_int_equal(ret, SR_ERR_OK);
        subscr = NULL;
    }

    ret = sr_session_stop(oper_sess);
    assert_int_equal(ret, SR_ERR_OK);
    return NULL;
}

static void
test_oper_sr_mon(void **arg)
{
    const int NUM_THREADS = test_full_scale ? 5 : 2;
    struct state *st = (struct state *)*arg;
    int i;
    pthread_t tid[NUM_THREADS];
    pthread_barrier_init(&st->barrier, NULL, NUM_THREADS);

    for (i = 0; i < NUM_THREADS; i++) {
        pthread_create(&tid[i], NULL, test_oper_srmon_helper, st);
    }

    for (i = 0; i < NUM_THREADS; i++) {
        pthread_join(tid[i], NULL);
    }
}


int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_leafref_oper_data),
        cmocka_unit_test(test_oper_sr_mon),
    };
    test_full_scale = getenv("TEST_FULL_SCALE") != NULL;
    setenv("CMOCKA_TEST_ABORT", "1", 1);
    test_log_init();
    return cmocka_run_group_tests(tests, setup_f, teardown_f);
}
