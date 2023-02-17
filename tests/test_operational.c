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

static int test_full_scale = 0;
static ATOMIC_T keep_running;

struct timespec start_time;

struct state
{
    sr_conn_ctx_t *conn;
    sr_session_ctx_t *sess;
    sr_session_ctx_t  *oper_sess;
    sr_subscription_ctx_t *subscr[2];
};

static int
setup_f(void **state)
{
    *state = NULL;
    sr_conn_ctx_t *conn = NULL;

    if (sr_connect(0, &conn) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_module(conn, TESTS_DIR "/files/ietf-interfaces.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_module(conn, TESTS_DIR "/files/iana-if-type.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
        return 1;
    }

    sr_disconnect(conn);

    clock_gettime(CLOCK_MONOTONIC, &start_time);
    return 0;
}

static int
teardown_f(void **state)
{
    (void)state;

    sr_conn_ctx_t *conn = NULL;

    if (sr_connect(0, &conn) != SR_ERR_OK) {
        return 1;
    }
    sr_remove_module(conn, "ietf-interfaces");
    sr_remove_module(conn, "iana-if-type");
    sr_disconnect(conn);
    return 0;
}

static void *
test_massive_scale_helper(void *arg)
{
    static ATOMIC_T ifid;
    (void)arg;
    const int TEST_RUN_SECONDS = 600;
    int ret;
    struct timespec now;
    struct state st;
    long id = ATOMIC_INC_RELAXED(ifid);
    char xpath[128];
    sr_conn_ctx_t *conn = NULL, *conn2 = NULL;

    do {
        memset(&st, 0, sizeof st);
        ret = sr_connect(0, &conn2);
        assert_int_equal(ret, SR_ERR_OK);

        ret = sr_session_start(conn2, SR_DS_RUNNING, &st.sess);
        assert_int_equal(ret, SR_ERR_OK);

        ret = sr_session_start(conn2, SR_DS_OPERATIONAL, &st.oper_sess);
        assert_int_equal(ret, SR_ERR_OK);

        /* Push some config into datastore */
        snprintf(xpath, sizeof xpath, "/ietf-interfaces:interfaces/interface[name='eth%lu']/type", id);
        ret = sr_set_item_str(st.sess, xpath, "iana-if-type:ethernetCsmacd", NULL, 0);
        assert_int_equal(ret, SR_ERR_OK);
        ret = sr_apply_changes(st.sess, 0, 1);
        assert_int_equal(ret, SR_ERR_OK);

        /* Push operational data into datastore */
        snprintf(xpath, sizeof xpath, "/ietf-interfaces:interfaces-state/interface[name='eth%lu']/type", id);

        ret = sr_set_item_str(st.oper_sess, xpath,
                "iana-if-type:ethernetCsmacd", NULL, SR_EDIT_STRICT);
        assert_int_equal(ret, SR_ERR_OK);
        ret = sr_apply_changes(st.oper_sess, 0, 1);
        assert_int_equal(ret, SR_ERR_OK);

       if (id) {
            /* Delete operational data */
            snprintf(xpath, sizeof xpath, "/ietf-interfaces:interfaces-state/interface[name='eth%lu']", id);
            ret = sr_delete_item(st.oper_sess, xpath, SR_EDIT_STRICT);
            TLOG_INF("Deleting oper data eth%lu", id);
            ret = sr_apply_changes(st.oper_sess, 0, 1);
            assert_int_equal(ret, SR_ERR_OK);
            usleep(100000);

            /* Push operational data into datastore */
            snprintf(xpath, sizeof xpath, "/ietf-interfaces:interfaces-state/interface[name='eth%lu']/type", id);

            ret = sr_set_item_str(st.oper_sess, xpath,
                    "iana-if-type:ethernetCsmacd", NULL, SR_EDIT_STRICT);
            assert_int_equal(ret, SR_ERR_OK);
            ret = sr_apply_changes(st.oper_sess, 0, 1);
            assert_int_equal(ret, SR_ERR_OK);

       } else {
            usleep(100000);
        }

       /* Delete config */
        snprintf(xpath, sizeof xpath, "/ietf-interfaces:interfaces/interface[name='eth%lu']", id);
        ret = sr_delete_item(st.sess, xpath, SR_EDIT_STRICT);
        ret = sr_apply_changes(st.sess, 0, 1);
        assert_int_equal(ret, SR_ERR_OK);

        sr_disconnect(conn2);
        conn2 = NULL;

        clock_gettime(CLOCK_MONOTONIC, &now);
        sched_yield();
    } while(now.tv_sec < start_time.tv_sec + TEST_RUN_SECONDS);

    return NULL;
}

static void
test_multi_thread_scale(void **arg)
{
    const int NUM_THREADS = 2;
    int i, ret;
    pthread_t tids[NUM_THREADS];
    (void)arg;

    for (i = 0; i < NUM_THREADS; i++) {
        ret = pthread_create(&tids[i], NULL, test_massive_scale_helper, NULL);
        assert_int_equal(ret, 0);
    }

    for (i = 0; i < NUM_THREADS; i++) {
        pthread_join(tids[i], NULL);
    }

}

int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_multi_thread_scale),
    };
    test_full_scale = getenv("TEST_FULL_SCALE") != NULL;
    setenv("CMOCKA_TEST_ABORT", "1", 1);
    test_log_init();
    return cmocka_run_group_tests(tests, setup_f, teardown_f);
}
