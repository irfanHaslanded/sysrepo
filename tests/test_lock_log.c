/**
 * @file test_lock_log.c
 * @author Irfan <irfan@graphiant.com>
 * @brief test for sr_rwlock_t logging
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
    int conn_per_thread;
    pthread_barrier_t barrier;
};

static int
setup_f(void **state)
{
    struct state *st;
    const char *schema_paths[] = {
        TESTS_SRC_DIR "/files/test.yang",
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
        "test",
        "test-leafref",
        NULL
    };

    if (sr_remove_modules(st->conn, module_names, 0) != SR_ERR_OK) {
        return 1;
    }

    sr_disconnect(st->conn);
    free(st);
    return 0;
}

typedef enum {
    TEST_CB_SLEEP = 0x1,
    TEST_CB_CRASH = 0x2,
} test_cb_action_t;

ATOMIC_T test_cb_action;
ATOMIC_T test_cb_sleep_ms;

static void
test_cb_act(test_cb_action_t action, uint32_t delay)
{
    if (action & TEST_CB_SLEEP) {
        TLOG_INF("%s sleeping %u ms", __func__, delay);
        usleep(delay * 1000u);
    }
    if (action & TEST_CB_CRASH) {
        TLOG_INF("%s Dying", __func__);
        exit(0);
    }
}

static int
module_action_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *module_name,
        const char *xpath, sr_event_t event, uint32_t request_id, void *private_data)
{
    (void)module_name;
    (void)sub_id;
    (void)xpath;
    (void)event;
    (void)request_id;
    (void)private_data;
    (void)session;
    test_cb_act(ATOMIC_LOAD_RELAXED(test_cb_action), ATOMIC_LOAD_RELAXED(test_cb_sleep_ms));
    return 0;
}

static int
test_oper_action_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *module_name, const char *xpath,
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
    test_cb_act(ATOMIC_LOAD_RELAXED(test_cb_action), ATOMIC_LOAD_RELAXED(test_cb_sleep_ms));

    return SR_ERR_OK;
}

static ATOMIC_T stop_threads;

static void *
test_lock_log_reader_limit_helper(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *sess = NULL;
    sr_data_t *data = NULL;
    int ret = 0;

    if (st->conn_per_thread) {
        ret = sr_connect(0, &conn);
        assert_int_equal(ret, SR_ERR_OK);
    } else {
        conn = st->conn;
    }

    ret = sr_session_start(conn, SR_DS_OPERATIONAL, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    /* weak allow all threads to spawn */
    usleep(200000);
    while (ret == SR_ERR_OK && !ATOMIC_LOAD_RELAXED(stop_threads)) {
        data = NULL;
        ret = sr_get_data(sess, "/test:*", 0, 0, 0, &data);
        sched_yield();
        sr_release_data(data);
    }
    TLOG_INF("%s Asking all threads to stop", __func__);
    ATOMIC_STORE_RELAXED(stop_threads, 1);
    sleep(1);
    ret = sr_session_stop(sess);
    assert_int_equal(ret, SR_ERR_OK);
    if (st->conn_per_thread) {
        sr_disconnect(conn);
    }
    return NULL;
}

static void
test_lock_log_reader_limit(void **arg)
{
    struct state *st = (struct state *)*arg;
    sr_subscription_ctx_t *subscr = NULL;
    size_t i, j;
    int ret;
    sr_data_t *data = NULL;
    const size_t NUM_THREADS = 2 + SR_RWLOCK_READ_LIMIT;
    pthread_t tid[NUM_THREADS];

    ATOMIC_STORE_RELAXED(stop_threads, 0);
    sr_session_start(st->conn, SR_DS_OPERATIONAL, &st->sess);

    ATOMIC_STORE_RELAXED(test_cb_action, TEST_CB_SLEEP);
    ATOMIC_STORE_RELAXED(test_cb_sleep_ms, 10);

    /* Create a subscription that will crash */
    ret = sr_oper_get_subscribe(st->sess, "test", "/test:*", test_oper_action_cb, NULL, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    for (j = 1; j < 2; j++) {
        st->conn_per_thread = j;
        /* Spawn more threads that will read than concurrent readers */
        for (i = 0; i < NUM_THREADS; i++)
        {
            pthread_create(&tid[i], NULL, test_lock_log_reader_limit_helper, st);
        }
        sleep(1 + j*1);
        ATOMIC_STORE_RELAXED(stop_threads, 1);
        for (i = 0; i < NUM_THREADS; i++)
        {
            pthread_join(tid[i], NULL);
        }

        TLOG_INF("%s All threads finished", __func__);
        /* Verify that after reader limit is reached, get succeeds eventually */
        ret = sr_get_data(st->sess, "/test:*", 0, 0, 0, &data);
        assert_int_equal(ret, SR_ERR_OK);
        sr_release_data(data);
    }

    ret = sr_unsubscribe(subscr);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_session_stop(st->sess);
    assert_int_equal(ret, SR_ERR_OK);
    st->sess = NULL;
}

static void
test_lock_cb_timeout(void **arg)
{
    struct state *st = (struct state *)*arg;
    sr_subscription_ctx_t *subscr = NULL;
    int i, ret;

    for (i = 0; i < 1; i++) {
        subscr = NULL;
        ret = sr_session_start(st->conn, SR_DS_RUNNING, &st->sess);
        assert_int_equal(ret, 0);

        ATOMIC_STORE_RELAXED(test_cb_action, TEST_CB_SLEEP);
        ATOMIC_STORE_RELAXED(test_cb_sleep_ms, 100 + SR_SHMEXT_SUB_LOCK_TIMEOUT);

        /* Start some subscriptions to test.yang */
        ret = sr_module_change_subscribe(st->sess, "test", NULL, module_action_cb, NULL,
                0, 0, &subscr);
        assert_int_equal(ret, 0);

        ret = sr_set_item_str(st->sess, "/test:l1[k='some-key']/v", "25", NULL, 0);
        assert_int_equal(ret, SR_ERR_OK);

        TLOG_INF("starting apply_changes");
        ret = sr_apply_changes(st->sess, 100);

        TLOG_INF("try unsubscribe, times out");
        ret = sr_unsubscribe(subscr);
        assert_int_equal(ret, SR_ERR_TIME_OUT);

        TLOG_INF("starting unsubscribe again");
        ret = sr_unsubscribe(subscr);
        assert_int_equal(ret, SR_ERR_OK);
        TLOG_INF("Succeeded unsubscribe on retry");

        ret = sr_session_stop(st->sess);
        assert_int_equal(ret, SR_ERR_OK);
        st->sess = NULL;
    }
}

static void
test_err_log(void **arg)
{
    struct state *st = (struct state *)*arg;
    int ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &st->sess);
    assert_int_equal(ret, 0);

    /* This forces us to print a message that contains a % which can trigger a format string vulnerability */
    ret = sr_set_item_str(st->sess, "/test-leafref:format-str-test", "120", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(st->sess, 100);
    assert_int_equal(ret, SR_ERR_VALIDATION_FAILED);

    ret = sr_session_stop(st->sess);
    assert_int_equal(ret, SR_ERR_OK);
    st->sess = NULL;
}

static void
test_lock_dead_reader_helper(void)
{
    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *sess = NULL;
    sr_data_t *data = NULL;
    sr_subscription_ctx_t *subscr = NULL;
    char xpath[128] = "";

    int ret;

    ret = sr_connect(0, &conn);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_session_start(conn, SR_DS_OPERATIONAL, &sess);
    assert_int_equal(ret, 0);

    snprintf(xpath, sizeof xpath, "/test-leafref:car-state/enabled-params[.='key%d']", getpid());

    ATOMIC_STORE_RELAXED(test_cb_action, TEST_CB_SLEEP | TEST_CB_CRASH);
    ATOMIC_STORE_RELAXED(test_cb_sleep_ms, 1000);

    /* Create a subscription that will crash */
    ret = sr_oper_get_subscribe(sess, "test-leafref", xpath, test_oper_action_cb, NULL, 0, &subscr);
    assert_true(ret == SR_ERR_TIME_OUT || ret == SR_ERR_OK);

    /* weak wait until every process gets here */
    sleep(1);
    TLOG_INF("Getting data");
    /* Now read our own data so we crash */
    ret = sr_get_data(sess, xpath, 0, 0, 0, &data);

    /* if we didn't crash in the callback,
     * because a lock blocked us from getting there, we will die now */
    exit(0);
    sr_release_data(data);
    data = NULL;

    sr_disconnect(conn);
}

static void
test_lock_dead_reader_limit(void **arg)
{
    struct state *st = (struct state *)*arg;
    int i, ret;
    const int NUM_PIDS = 2 * SR_RWLOCK_READ_LIMIT;
    pid_t pid;
    sr_data_t *data = NULL;

    for (i = 0; i < NUM_PIDS; i++) {
        switch (pid = fork()) {
            case 0:
                /* child */
                test_lock_dead_reader_helper();
                /* should never come here */
                TLOG_WRN("%s: never come here", __func__);
                abort();
            default:
                assert_int_not_equal(pid, -1);
                break;
        }
    }
    for (i = 0; i < NUM_PIDS; i++) {
        pid = wait(&ret);
        ret = WEXITSTATUS(ret);
        TLOG_INF("%s ret %x pid %d", __func__, ret, pid);
        // TODO FIXME this does not work with valgrind
        // assert_int_equal(ret, 0);
    }
    /* Now that all the processes which were supposed to die, have died, ensure that we actually can read again */

    TLOG_INF("Check that we can still read data");
    ret = sr_session_start(st->conn, SR_DS_OPERATIONAL, &st->sess);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_get_data(st->sess, "/*", 0, 0, 0, &data);
    assert_int_equal(ret, SR_ERR_OK);

    sr_release_data(data);

    ret = sr_session_stop(st->sess);
    assert_int_equal(ret, SR_ERR_OK);
    st->sess = NULL;
}

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_lock_cb_timeout),
    };
    const struct CMUnitTest tests2[] = {
        cmocka_unit_test(test_lock_log_reader_limit),
        cmocka_unit_test(test_err_log),
        cmocka_unit_test(test_lock_cb_timeout),
        cmocka_unit_test(test_lock_dead_reader_limit),
    };

    (void)tests2;
    setenv("CMOCKA_TEST_ABORT", "1", 1);
    setenv("SR_TEST_LOG_DEBUG", "1", 1);
    test_log_init();
    return cmocka_run_group_tests(tests, setup_f, teardown_f);
}
