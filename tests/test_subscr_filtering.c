
/**
 * @file test_subscr_filtering.c
 * @author Irfan <irfan@graphiant.com>
 * @brief test for optimization of subscription filtering
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

#include <cmocka.h>
#include <libyang/libyang.h>

#include "sysrepo.h"
#include "common.h"
#include "tests/test_common.h"

struct state {
    sr_conn_ctx_t *conn;
    sr_session_ctx_t *sess;
    ATOMIC_T running;
    pthread_barrier_t barrier;
};

static int
setup_f(void **state)
{
    struct state *st;

    st = calloc(1, sizeof *st);
    if (!st) {
        return 1;
    }
    *state = st;

    if (sr_connect(0, &st->conn) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_module(st->conn, TESTS_DIR "/files/test.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
        return 1;
    }
    sr_disconnect(st->conn);

    if (sr_connect(0, &st->conn) != SR_ERR_OK) {
        return 1;
    }

    return 0;
}

static int
teardown_f(void **state)
{
    struct state *st = (struct state *)*state;

    sr_remove_module(st->conn, "test");

    sr_disconnect(st->conn);
    free(st);
    return 0;
}

static int
module_change_deadlock_cb(sr_session_ctx_t *session, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t request_id, void *private_data)
{
    (void)session;
    (void)module_name;
    (void)xpath;
    (void)event;
    (void)request_id;
    (void)private_data;
    sched_yield();
    return 0;
}

static void *
try_get_oper_data(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess = NULL;
    int ret;
    uint32_t iter = 0;

    struct lyd_node *data = NULL;
    ret = sr_session_start(st->conn, SR_DS_OPERATIONAL, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    while (ATOMIC_LOAD_RELAXED(st->running)) {
        sched_yield();
        data = NULL;
        ++iter;
        ret = sr_get_data(sess, "/test:*", 0, 0, 0, &data);
        assert_int_equal(ret, SR_ERR_OK);
        lyd_free_withsiblings(data);
    }

    return NULL;
}

static void
test_subscr_enable_deadlock(void **arg)
{
    struct state *st = (struct state *)*arg;
    int ret;
    const int num_iters = 10000;

    pthread_t tid;
    sr_subscription_ctx_t *subscr = NULL;
    sr_subscr_options_t opts = SR_SUBSCR_ENABLED;

    sr_session_start(st->conn, SR_DS_RUNNING, &st->sess);

    TLOG_INF("Step 1. Add some init_data");
    ret = sr_set_item_str(st->sess, "/test:l1[k='init_data']/v", "1", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(st->sess, 0, 1);
    assert_int_equal(ret, SR_ERR_OK);

    ATOMIC_STORE_RELAXED(st->running, 1);
    pthread_create(&tid, NULL, try_get_oper_data, st);
    sleep(3);
    TLOG_INF("Step 2. Add a subscriber with enabled flag which will make more changes to the module");
    for (int i = 0; i < num_iters; i++) {
        TLOG_INF("sub unsub iter %u", i);
        ret = sr_module_change_subscribe(st->sess, "test", NULL, module_change_deadlock_cb, st,
                0, opts, &subscr);
        assert_int_equal(ret, SR_ERR_OK);
        ret = sr_unsubscribe(subscr);
        assert_int_equal(ret, SR_ERR_OK);
    }
    ATOMIC_STORE_RELAXED(st->running, 0);

    ret = sr_delete_item(st->sess, "/test:l1[k='init_data']/v", 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(st->sess, 0, 1);
    assert_int_equal(ret, SR_ERR_OK);


    pthread_join(tid, NULL);
}

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_subscr_enable_deadlock),
    };
    setenv("CMOCKA_TEST_ABORT", "1", 1);
    test_log_init();
    return cmocka_run_group_tests(tests, setup_f, teardown_f);
}
