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
extern int sr_test_insert_delay_before_unlock;

struct state {
    sr_conn_ctx_t *conn;
    sr_session_ctx_t *sess;
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
    if (sr_install_module(st->conn, TESTS_SRC_DIR "/files/ietf-interfaces.yang", TESTS_SRC_DIR "/files", NULL) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_module(st->conn, TESTS_SRC_DIR "/files/iana-if-type.yang", TESTS_SRC_DIR "/files", NULL) != SR_ERR_OK) {
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

    sr_remove_module(st->conn, "ietf-interfaces", 0);
    sr_remove_module(st->conn, "iana-if-type", 0);

    sr_disconnect(st->conn);
    free(st);
    return 0;
}

static int
oper_first_cb(sr_session_ctx_t *session, uint32_t sub_id, const char *module_name, const char *path,
        const char *request_xpath, uint32_t request_id, struct lyd_node **parent, void *private_data)
{
    (void)session;
    (void)private_data;
    (void)sub_id;
    (void)module_name;
    (void)path;
    (void)request_xpath;
    (void)request_id;
    (void)parent;
    static int first_time = 1;
    assert_string_equal(module_name, "ietf-interfaces");
    assert_non_null(parent);
    if (first_time) {
        first_time = 0;
        TLOG_INF("Sleeping to cause timeout");
        usleep(110000);
    }
    return SR_ERR_OK;
}

static void*
test_oper_pull_thread(void *arg)
{
    sr_data_t *data = NULL;
    struct state *st = (struct state *)arg;
    int ret, i = 0;
    sr_conn_ctx_t *conn;
    sr_session_ctx_t *sess = NULL;

    ret = sr_connect(0, &conn);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_session_start(conn, SR_DS_OPERATIONAL, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    sr_test_insert_delay_before_unlock = 1;

    /* Try to fetch a lot of data which times out */
    for (i = 0; i < 10; i++) {
        ret = sr_get_data(sess, "/ietf-interfaces:interfaces-state/*", 0, 100 + (i%2)*1000, SR_OPER_WITH_ORIGIN, &data);
        // assert_int_equal(1,  ret == SR_ERR_OK || ret == SR_ERR_TIME_OUT);
        sr_release_data(data);
        /* subsequent calls should all pass */
        sr_test_insert_delay_before_unlock = 0;
    }
    sr_disconnect(conn);
    return NULL;
}

static void
test_oper_pull(void **arg)
{
    const int NUM_THREADS = 1;
    struct state *st = (struct state *)*arg;
    int i, ret;
    pthread_t tid[NUM_THREADS];
    sr_subscription_ctx_t *subscr = NULL;

    sr_session_start(st->conn, SR_DS_OPERATIONAL, &st->sess);

    /* Set a subscriber for providing oper data */
    ret = sr_oper_get_subscribe(st->sess, "ietf-interfaces", "/ietf-interfaces:interfaces-state/*", oper_first_cb,
            NULL, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    for (i = 0; i < NUM_THREADS; i++) {
        pthread_create(&tid[i], NULL, test_oper_pull_thread, st);
    }

    for (i = 0; i < NUM_THREADS; i++) {
        pthread_join(tid[i], NULL);
    }
    sr_unsubscribe(subscr);
}

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_oper_pull),
    };
    test_full_scale = getenv("TEST_FULL_SCALE") != NULL;
    setenv("CMOCKA_TEST_ABORT", "1", 1);
    test_log_init();
    return cmocka_run_group_tests(tests, setup_f, teardown_f);
}
