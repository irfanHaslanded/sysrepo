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
#include "tests/test_common.h"


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
    if (sr_install_module(st->conn, TESTS_DIR "/files/test-leafref.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
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

    sr_remove_module(st->conn, "test-leafref");

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
    struct lyd_node *data = NULL;

    const char *xpath1 = "/test-leafref:car/parameters[name='temperature']";
    const char *xpath1_del = "/test-leafref:car/parameters";

    const char *xpath2 = "/test-leafref:car/parameters[name='pressure']";

    const char *xpath_oper_del = "/test-leafref:car-state/enabled-params";

    const char *a1 = "/test-leafref:car-state/a1";
    const char *xo1 = "/test-leafref:car-state/enabled-params[.='temperature']";
    const char *xo1_del = "/test-leafref:car-state/enabled-params[.='temperature']";

    const char *xo2 =  "/test-leafref:car-state/enabled-params[.='pressure']";
    const char *xo2_del = "/test-leafref:car-state/enabled-params[.='pressure']";

    const char *v1 = "10 C";
    const char *v2 = "1 atm";

    ret = sr_connect(0, &conn);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_session_start(conn, SR_DS_RUNNING, &st->sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_session_start(st->conn, SR_DS_OPERATIONAL, &oper_sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(st->sess, xpath1, NULL, NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(st->sess, xpath2, NULL, NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(st->sess, 0, 1);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(oper_sess, a1, "", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_delete_item(oper_sess, xo2_del, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(oper_sess, xo2, v2, NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_delete_item(oper_sess, xo1_del, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(oper_sess, xo1, v1, NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_delete_item(oper_sess, xo2_del, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(oper_sess, xo2, v2, NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(oper_sess, 0, 1);
    assert_int_equal(ret, SR_ERR_OK);
    TLOG_INF("Added oper data");

    ret = sr_delete_item(oper_sess, xo1_del, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_delete_item(oper_sess, xo2_del, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(oper_sess, 0, 1);
    assert_int_equal(ret, SR_ERR_OK);
    TLOG_INF("Removed oper data");

    ret = sr_get_data(oper_sess, "/test-leafref:*", 0, 0, 0, &data);
    assert_int_equal(ret, SR_ERR_OK);
    lyd_free_withsiblings(data);
    data = NULL;


    ret = sr_session_stop(oper_sess);
    assert_int_equal(ret, SR_ERR_OK);

    sr_disconnect(conn);
}



int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_leafref_oper_data),
    };
    setenv("CMOCKA_TEST_ABORT", "1", 1);
    test_log_init();
    return cmocka_run_group_tests(tests, setup_f, teardown_f);
}
