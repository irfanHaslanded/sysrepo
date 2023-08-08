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

static int
setup_f(void **state)
{
    struct state *st;
    const char *schema_paths[] = {
        TESTS_SRC_DIR "/files/iana-if-type.yang",
        TESTS_SRC_DIR "/files/ietf-if-aug.yang",
        TESTS_SRC_DIR "/files/ex-interfaces.yang",
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
        "ex-interfaces",
        "ietf-if-aug",
        "iana-if-type",
        NULL
    };

    sr_remove_modules(st->conn, module_names, 0);

    sr_disconnect(st->conn);
    free(st);
    return 0;
}

static void
test_oper_mixed(void **arg)
{
    struct state *st = (struct state *)*arg;
    sr_data_t *data = NULL;
    int ret;
    sr_conn_ctx_t *conn = NULL;
    sr_session_ctx_t *oper_sess = NULL;

    ret = sr_connect(0, &conn);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_session_start(st->conn, SR_DS_OPERATIONAL, &st->sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_session_start(conn, SR_DS_OPERATIONAL, &oper_sess);
    assert_int_equal(ret, SR_ERR_OK);

    // Create the interface
    ret = sr_set_item_str(st->sess, "/ex-interfaces:interfaces/interface[name=\'mixed\']",
            NULL, NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(st->sess, "/ex-interfaces:interfaces/interface[name=\'mixed\']/type",
            "ethernetCsmacd", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_delete_item(st->sess, "/ex-interfaces:interfaces/interface[name=\'mixed\']/speed", 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(st->sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(st->sess, "/ex-interfaces:interfaces/interface[name=\'mixed\']/speed",
            "0", NULL, SR_EDIT_ISOLATE);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(st->sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_get_data(oper_sess, "/ex-interfaces:interfaces/interface[name=\'mixed\']/type", 0, 0, 0, &data);
    assert_int_equal(ret, SR_ERR_OK);

    sr_release_data(data);
    data = NULL;

    ret = sr_delete_item(st->sess, "/ex-interfaces:interfaces/interface[name=\'mixed\']", 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(st->sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    sr_disconnect(conn);
}



int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_oper_mixed),
    };
    setenv("CMOCKA_TEST_ABORT", "1", 1);
    test_log_init();
    return cmocka_run_group_tests(tests, setup_f, teardown_f);
}
