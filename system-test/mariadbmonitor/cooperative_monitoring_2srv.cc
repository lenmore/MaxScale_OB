/*
 * Copyright (c) 2024 MariaDB plc, Finnish Branch
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2027-04-10
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <maxtest/testconnections.hh>
#include <maxbase/string.hh>
#include "mariadbmon_utils.hh"

using std::string;
using namespace cooperative_monitoring;

namespace
{

void test_main(TestConnections& test)
{
    test.expect(test.n_maxscales() >= 2, "At least 2 MaxScales are needed for this test. Exiting");
    if (!test.ok())
    {
        return;
    }

    const auto master_slave = {mxt::ServerInfo::master_st, mxt::ServerInfo::slave_st};
    const auto slave_master = {mxt::ServerInfo::slave_st, mxt::ServerInfo::master_st};
    auto& mxs1 = *test.maxscale;
    auto& mxs2 = *test.maxscale2;
    auto& repl = *test.repl;

    mxs1.start_maxscale();
    // Ensure MaxScale1 gets locks.
    mxs1.wait_for_monitor(1);
    mxs2.start_maxscale();
    mxs2.wait_for_monitor(1);

    MonitorInfo monitors[] = {
        {1,  "MariaDB-Monitor"},
        {2,  "MariaDB-Monitor"},
        {-1, "none"           },
    };

    monitors[0].maxscale = &mxs1;
    monitors[1].maxscale = &mxs2;

    auto wait_both = [&monitors](int ticks) {
        for (int i = 0; i < ticks; i++)
        {
            monitors[0].maxscale->wait_for_monitor(1);
            monitors[1].maxscale->wait_for_monitor(1);
        }
    };

    const auto* primary_mon = get_primary_monitor(test, monitors);
    test.expect(primary_mon && primary_mon->id == 1, "MaxScale1 does not have exclusive lock.");

    mxs1.check_print_servers_status(master_slave);
    mxs2.check_print_servers_status(master_slave);

    if (test.ok())
    {
        test.tprintf("Stop master for 2 seconds, then bring it back. Primary MaxScale and master should "
                     "not change.");
        auto* srv1 = repl.backend(0);
        srv1->stop_database();
        sleep(2);
        srv1->start_database();
        mxs1.wait_for_monitor(2);
        mxs2.wait_for_monitor();

        primary_mon = get_primary_monitor(test, monitors);
        test.expect(primary_mon && primary_mon->id == 1,
                    "MaxScale1 does not have exclusive locks after server1 restart.");
        mxs1.check_print_servers_status(master_slave);
        mxs2.check_print_servers_status(master_slave);

        test.tprintf("Stop master for several monitor ticks, then bring it back. Server2 should get "
                     "promoted in the meantime.");
        srv1->stop_database();
        wait_both(4);

        for (int i = 0; i < 3; i++)
        {
            if (mxs1.get_servers().get(1).status == mxt::ServerInfo::master_st)
            {
                break;
            }
            sleep(1);
        }
        srv1->start_database();
        mxs1.wait_for_monitor(2);
        mxs2.wait_for_monitor();

        primary_mon = get_primary_monitor(test, monitors);
        test.expect(primary_mon && primary_mon->id == 1,
                    "MaxScale1 does not have exclusive lock after server1 failover.");
        mxs1.check_print_servers_status(slave_master);
        mxs2.check_print_servers_status(slave_master);

        if (test.ok())
        {
            test.log_printf("Block server2 and wait a few seconds. Primary monitor should not change. "
                            "Server1 should be promoted master.");
            int block_server_ind = 1;
            repl.block_node(block_server_ind);
            sleep(2);

            auto get_lock_owner = [&]() {
                auto* srv2 = repl.backend(block_server_ind);
                string query = R"(SELECT IS_USED_LOCK(\"maxscale_mariadbmonitor_master\"))";
                auto res = srv2->vm_node().run_sql_query(query);
                test.tprintf("Query '%s' returned %i: '%s'", query.c_str(), res.rc, res.output.c_str());
                test.expect(res.rc == 0, "Query failed.");
                int conn_id = -1;
                mxb::get_int(res.output, &conn_id);
                return conn_id;
            };
            int lock_owner = get_lock_owner();
            test.expect(lock_owner > 0, "Lock on server2 released faster than expected.");

            auto mon1 = monitors[0];
            for (int i = 0; i < 5; i++)
            {
                wait_both(1);
                test.expect(monitor_is_primary(test, mon1),
                            "MaxScale %i does not have exclusive lock after server2 was blocked.",
                            mon1.id);

                if (mxs1.get_servers().get(0).status == mxt::ServerInfo::master_st)
                {
                    break;
                }
            }

            auto master_down = {mxt::ServerInfo::master_st, mxt::ServerInfo::DOWN};
            mxs1.check_print_servers_status(master_down);

            test.tprintf("Launching failover should have taken longer than wait_timeout (6 seconds), "
                         "causing server2 to disconnect the monitor, releasing any locks.");
            lock_owner = get_lock_owner();
            if (lock_owner > 0)
            {
                test.add_failure("Lock is still owned by connection %i.", lock_owner);
            }
            else
            {
                test.tprintf("Lock is free on server2.");
            }

            // MaxScale2 may need some extra time to detect the new master as it's waiting for server1 to
            // become invalid.
            for (int i = 0; i < 5; i++)
            {
                if (mxs2.get_servers().get(0).status == mxt::ServerInfo::master_st)
                {
                    break;
                }
                else
                {
                    sleep(1);
                }
            }

            mxs2.check_print_servers_status(master_down);

            test.tprintf("Unblock server2. MaxScale1 should remain primary as it already had one lock.");
            repl.unblock_node(block_server_ind);
            sleep(1);
            wait_both(1);
            test.expect(monitor_is_primary(test, mon1), "MaxScale1 is not primary");

            mxs1.check_print_servers_status(master_slave);
            mxs2.check_print_servers_status(master_slave);
        }
    }
}
}

int main(int argc, char* argv[])
{
    TestConnections test;
    TestConnections::skip_maxscale_start(true);
    return test.run_test(argc, argv, test_main);
}
