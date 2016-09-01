/**
 * @file application_example.cpp
 * @author Mislav Novakovic <mislav.novakovic@sartura.hr>
 * @brief Example application that uses sysrepo as the configuraton datastore.
 *
 * @copyright
 * Copyright 2016 Deutsche Telekom AG.
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

#include <unistd.h>
#include <iostream>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>

#include "Session.h"

#define MAX_LEN 100

using namespace std;

volatile int exit_application = 0;

void
print_value(shared_ptr<Val> value)
{
    cout << value->xpath();
    cout << " ";

    switch (value->type()) {
    case SR_CONTAINER_T:
    case SR_CONTAINER_PRESENCE_T:
        cout << "(container)" << endl;
        break;
    case SR_LIST_T:
        cout << "(list instance)" << endl;
        break;
    case SR_STRING_T:
        cout << "= " << value->data()->get_string() << endl;;
        break;
    case SR_BOOL_T:
	if (value->data()->get_bool())
            cout << "= true" << endl;
	else
            cout << "= false" << endl;
        break;
    case SR_ENUM_T:
        cout << "= " << value->data()->get_enum() << endl;;
        break;
    case SR_UINT8_T:
        cout << "= " << unsigned(value->data()->get_uint8()) << endl;
        break;
    case SR_UINT16_T:
        cout << "= " << unsigned(value->data()->get_uint16()) << endl;
        break;
    case SR_UINT32_T:
        cout << "= " << unsigned(value->data()->get_uint32()) << endl;
        break;
    case SR_UINT64_T:
        cout << "= " << unsigned(value->data()->get_uint64()) << endl;
        break;
    case SR_INT8_T:
        cout << "= " << value->data()->get_int8() << endl;
        break;
    case SR_INT16_T:
        cout << "= " << value->data()->get_int16() << endl;
        break;
    case SR_INT32_T:
        cout << "= " << value->data()->get_int32() << endl;
        break;
    case SR_INT64_T:
        cout << "= " << value->data()->get_int64() << endl;
        break;
     case SR_IDENTITYREF_T:
        cout << "= " << value->data()->get_identityref() << endl;
        break;
    case SR_BITS_T:
        cout << "= " << value->data()->get_bits() << endl;
        break;
    case SR_BINARY_T:
        cout << "= " << value->data()->get_binary() << endl;
        break;
    default:
        cout << "(unprintable)" << endl;
    }
    return;
}

static void
print_current_config(shared_ptr<Session> session, const char *module_name)
{
    char select_xpath[MAX_LEN];
    try {
        snprintf(select_xpath, MAX_LEN, "/%s:*//*", module_name);

        auto values = session->get_items(&select_xpath[0]);
        if (values == NULL)
            return;

        for(unsigned int i = 0; i < values->val_cnt(); i++)
            print_value(values->val(i));
    } catch( const std::exception& e ) {
        cout << e.what() << endl;
    }
}

static int
module_change_cb(sr_session_ctx_t *session, const char *module_name, sr_notif_event_t event, \
                 void *private_ctx)
{
    cout << "\n\n ========== CONFIG HAS CHANGED, CURRENT RUNNING CONFIG: ==========\n" << endl;

    shared_ptr<Session> sess(new Session(session));
    print_current_config(sess, module_name);

    return SR_ERR_OK;
}

static void
sigint_handler(int signum)
{
    exit_application = 1;
}

int
main(int argc, char **argv)
{
    const char *module_name = "ietf-interfaces";
    try {
        if (argc > 1) {
            module_name = argv[1];
        } else {
            printf("\nYou can pass the module name to be subscribed as the first argument\n");
        }

        printf("Application will watch for changes in %s\n", module_name);
        shared_ptr<Connection> conn(new Connection("examples_application"));

        shared_ptr<Session> sess(new Session(conn));

        /* read startup config */
        cout << "\n\n ========== READING STARTUP CONFIG: ==========\n" << endl;

        shared_ptr<Subscribe> subscribe(new Subscribe(sess));

        subscribe->module_change_subscribe(module_name, module_change_cb);

        print_current_config(sess, module_name);

        cout << "\n\n ========== STARTUP CONFIG APPLIED AS RUNNING ==========\n" << endl;

        /* loop until ctrl-c is pressed / SIGINT is received */
        signal(SIGINT, sigint_handler);
        while (!exit_application) {
            sleep(1000);  /* or do some more useful work... */
        }

        printf("Application exit requested, exiting.\n");

    } catch( const std::exception& e ) {
        cout << e.what() << endl;
        return -1;
    }

    return 0;
}
