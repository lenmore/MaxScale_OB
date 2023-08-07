/*
 * Copyright (c) 2022 MariaDB Corporation Ab
 * Copyright (c) 2023 MariaDB plc, Finnish Branch
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2027-07-24
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "auth_utils.hh"
#include <maxbase/format.hh>

namespace
{
const char connector_plugin_dir[] = "../../connector-c/install/lib/mariadb/plugin";
const char basic_pam_cfg[] = "pam_config_simple";
const char pam_cfg_loc[] = "/etc/pam.d/%s";
}

using std::string;

namespace auth_utils
{
void try_conn(TestConnections& test, int port, Ssl ssl, const string& user, const string& pass,
              bool expect_success)
{
    mxt::MariaDB maxconn(test.logger());
    auto& sett = maxconn.connection_settings();
    sett.plugin_dir = connector_plugin_dir;
    sett.user = user;
    sett.password = pass;
    sett.ssl.enabled = ssl == Ssl::ON;

    const string& host = test.maxscale->ip4();

    test.tprintf("Trying to log in to [%s]:%i as '%s' using password '%s'.", host.c_str(), port,
                 user.c_str(), pass.c_str());
    bool connected = maxconn.try_open(host, port);
    if (connected)
    {
        if (expect_success)
        {
            auto res = maxconn.query("select rand();");
            if (!res || !res->next_row())
            {
                test.add_failure("Test query failed: %s", maxconn.error());
            }
        }
        else
        {
            test.add_failure("Connection to MaxScale succeeded when failure was expected.");
        }
    }
    else if (expect_success)
    {
        test.add_failure("Connection to MaxScale failed: %s", maxconn.error());
    }
    else
    {
        test.tprintf("Connection to MaxScale failed as expected.");
    }
}

void copy_basic_pam_cfg(mxt::MariaDBServer* server)
{
    // Copy pam config.
    string pam_config_path_src = mxb::string_printf("%s/authentication/%s", mxt::SOURCE_DIR, basic_pam_cfg);
    string pam_config_path_dst = mxb::string_printf(pam_cfg_loc, basic_pam_cfg);
    server->vm_node().copy_to_node_sudo(pam_config_path_src, pam_config_path_dst);
}

void remove_basic_pam_cfg(mxt::MariaDBServer* server)
{
    string pam_config_path_dst = mxb::string_printf(pam_cfg_loc, basic_pam_cfg);
    server->vm_node().delete_from_node(pam_config_path_dst);
}

void create_basic_pam_user(mxt::MariaDBServer* server, const string& user, const std::string& pw)
{
    server->admin_connection()->cmd_f("create or replace user %s identified via pam using '%s';",
                                      user.c_str(), basic_pam_cfg);
    server->vm_node().add_linux_user(user, pw);
}

void delete_basic_pam_user(mxt::MariaDBServer* server, const string& user)
{
    server->admin_connection()->cmd_f("drop user %s;", user.c_str());
    server->vm_node().remove_linux_user(user);
}
}
