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

static int test_full_scale = 1;

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
recursive_oper_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *module_name, const char *xpath, const char *request_xpath,
        uint32_t request_id, struct lyd_node **parent, void *private_data)
{
    sr_data_t *tmp = NULL;
    const char *get_xpath = "/test:*";
    (void)session;
    (void)sub_id;
    (void)module_name;
    (void)xpath;
    (void)request_xpath;
    (void)request_id;
    (void)parent;
    (void)private_data;
    int ret;

    if (!strcmp(module_name, "test")) {
        get_xpath = "/test-leafref:*";
    }
    ret = sr_get_data(session, get_xpath, 0, 0, 0, &tmp);
    assert_int_equal(ret, SR_ERR_OK);
    sr_release_data(tmp);
    return SR_ERR_OK;
}

static int
module_change_recursive_get_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t request_id, void *private_data)
{
    sr_data_t *tmp = NULL;
    const char *get_xpath = "/test:*";
    (void)session;
    (void)sub_id;
    (void)module_name;
    (void)xpath;
    (void)event;
    (void)request_id;
    int ret;

    TLOG_INF("%s for %s", __func__, xpath);

    cb_stats *stats = (cb_stats *)private_data;
    ATOMIC_INC_RELAXED(stats->count);
    if (!strcmp(module_name, "test")) {
        get_xpath = "/test-leafref:*";
    }
    ret = sr_get_data(session, get_xpath, 0, 0, 0, &tmp);
    assert_int_equal(ret, SR_ERR_OK);
    sr_release_data(tmp);

    return 0;
}

static void *
test_massive_scale_helper(void *arg)
{
    const size_t NUM_XPATHS = test_full_scale ? 10 : 1;
    size_t NUM_THREADS;
    static ATOMIC_T thread_id;
    size_t i, id = ATOMIC_INC_RELAXED(thread_id);
    int ret;
    char xpath[128];
    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *sess = NULL, *oper_sess = NULL;
    struct state *st = (struct state *)arg;
    sr_subscription_ctx_t *subscr[2] = {NULL};
    sr_subscr_options_t opts = SR_SUBSCR_ENABLED;
    size_t j;
    cb_stats stats[2] = {0};
    size_t count = 0;
    pthread_t thread;
    cpu_set_t cpuset;

    thread = pthread_self();

    CPU_ZERO(&cpuset);
    CPU_SET(id%8, &cpuset);
    ret = pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset);
    if (ret != 0) {
        TLOG_WRN("pthread_setaffinity(%d): %s", id%8, strerror(ret));
    }

    sr_data_t *data = NULL;

    ret = sr_connect(0, &conn);
    assert_int_equal(ret, SR_ERR_OK);

    /* All threads are up */
    NUM_THREADS = ATOMIC_LOAD_RELAXED(thread_id);

    while (ATOMIC_LOAD_RELAXED(st->running)) {
        TLOG_INF("Starting invocation %u", ++count);
        subscr[0] = NULL;
        subscr[1] = NULL;
        sess = NULL;
        oper_sess = NULL;

        TLOG_INF("Starting sessions");
        ret = sr_session_start(conn, SR_DS_RUNNING, &sess);
        assert_int_equal(ret, SR_ERR_OK);

        ret = sr_session_start(conn, SR_DS_OPERATIONAL, &oper_sess);
        assert_int_equal(ret, SR_ERR_OK);

        TLOG_INF("Starting subscriptions");
        /* Step 1. create a subscription for module test */
        for (i = 0; i < NUM_XPATHS; i++) {
            TLOG_INF("Starting subscr xpath %d", i);
            j = id*NUM_XPATHS + i;
            snprintf(xpath, sizeof xpath, "/test:l1[k='key%lu']/v", j);
            ret = sr_module_change_subscribe(sess, "test", xpath, module_change_recursive_get_cb, &stats[0],
                          0, opts, &subscr[0]);
            assert_int_equal(ret, SR_ERR_OK);
        }
        TLOG_INF("Starting oper pull subscr");
        /* Step 2. create a an oper pull subscription for module test */
        snprintf(xpath, sizeof xpath, "/test-leafref:car-state/enabled-params[.='key%lu']", id);

        ret = sr_oper_get_subscribe(oper_sess, "test-leafref", xpath, recursive_oper_cb, NULL, 0, &subscr[1]);
        assert_int_equal(ret, SR_ERR_OK);
        /* Step 3. change some running data in module test */
        for (i = 0; i < NUM_XPATHS; i++) {
            j = ((1 + id) % NUM_THREADS) * NUM_XPATHS + i;
            snprintf(xpath, sizeof xpath, "/test:l1[k='key%lu']/v", j);
            ret = sr_set_item_str(sess, xpath, "1", NULL, 0);
            assert_int_equal(ret, SR_ERR_OK);
        }
        TLOG_INF("Starting sr_apply_changes");
        ret = sr_apply_changes(sess, 0);
        assert_int_equal(ret, SR_ERR_OK);
        /* Step 4. read some operational data in module test */

        TLOG_INF("Starting oper get");
        /* Fetch entire operational datastore incl sysrepo-monitoring */
        ret = sr_get_data(oper_sess, "/*", 0, 0, 0, &data);
        assert_int_equal(ret, SR_ERR_OK);
        sr_release_data(data);
        data = NULL;
        /* Step 5. delete running data in module test */
        for (i = 0; i < NUM_XPATHS; i++) {
            j = ((1 + id) % NUM_THREADS) * NUM_XPATHS + i;
            snprintf(xpath, sizeof xpath, "/test:l1[k='key%lu']/v", j);
            ret = sr_delete_item(sess, xpath, 0);
            assert_int_equal(ret, SR_ERR_OK);
        }

        TLOG_INF("Starting delete running");
        ret = sr_apply_changes(sess, 0);
        assert_int_equal(ret, SR_ERR_OK);
        /* Step 6. stop the operational pull subscription */
        TLOG_INF("Stopping oper subscr");
        ret = sr_unsubscribe(subscr[1]);
        assert_int_equal(ret, SR_ERR_OK);

        /* Step 7. stop the running subscription */
        TLOG_INF("Stopping change subscr");
        ret = sr_unsubscribe(subscr[0]);
        assert_int_equal(ret, SR_ERR_OK);

        TLOG_INF("Stopping sessions");
        /* Step 8. stop the sessions */
        ret = sr_session_stop(sess);
        assert_int_equal(ret, SR_ERR_OK);

        ret = sr_session_stop(oper_sess);
        assert_int_equal(ret, SR_ERR_OK);
    }
    sr_disconnect(conn);
    return NULL;
}

static void
test_massive_scale(void **arg)
{
    size_t i, NUM_THREADS = test_full_scale ? 2 : 1;
    pthread_t tid[NUM_THREADS];
    struct state *st = (struct state *)*arg;
    ATOMIC_STORE_RELAXED(st->running, 1);
    for (i = 0; i < NUM_THREADS; i++) {
        pthread_create(&tid[i], NULL, test_massive_scale_helper, st);
    }
    sleep(60);
    ATOMIC_STORE_RELAXED(st->running, 0);
    for (i = 0; i < NUM_THREADS; i++) {
        pthread_join(tid[i], NULL);
    }
}

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_massive_scale),
    };

    setenv("CMOCKA_TEST_ABORT", "1", 1);
    test_log_init();
    return cmocka_run_group_tests(tests, setup_f, teardown_f);
}
