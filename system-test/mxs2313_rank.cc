/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 * Copyright (c) 2023 MariaDB plc, Finnish Branch
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2025-09-12
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

/**
 * MXS-2313: `rank` functional tests
 * https://jira.mariadb.org/browse/MXS-2313
 */

#include <maxtest/testconnections.hh>
#include <iostream>
#include <fstream>

std::function<void(std::vector<std::string>)> set_states;

auto RW = "master running";
auto RO = "slave running";
auto DOWN = "maintenance running";

void test_rwsplit(TestConnections& test, std::vector<std::string> ids)
{
    std::cout << "Servers in two groups with different ranks" << std::endl;

    test.check_maxctrl("alter server server1 rank primary");
    test.check_maxctrl("alter server server2 rank primary");
    test.check_maxctrl("alter server server3 rank secondary");
    test.check_maxctrl("alter server server4 rank secondary");

    Connection c = test.maxscale->rwsplit();

    auto is_primary = [&]() {
        auto id = c.field("SELECT @@server_id");
        test.expect(id == ids[0] || id == ids[1], "Primary servers should reply");
    };
    auto is_secondary = [&]() {
        auto id = c.field("SELECT @@server_id");
        test.expect(id == ids[2] || id == ids[3], "Secondary servers should reply");
    };

    set_states({RW, RO, RO, RO});

    c.connect();
    is_primary();

    set_states({DOWN, RW, RO, RO});
    is_primary();

    set_states({DOWN, DOWN, RW, RO});
    is_secondary();

    set_states({DOWN, DOWN, DOWN, RW});
    is_secondary();

    set_states({DOWN, DOWN, DOWN, DOWN});
    test.expect(!c.query("SELECT @@server_id"), "Query should fail");

    set_states({DOWN, DOWN, DOWN, RW});
    c.disconnect();
    c.connect();
    is_secondary();

    set_states({DOWN, DOWN, RW, RO});
    is_secondary();

    set_states({DOWN, RW, RO, RO});
    is_secondary();

    set_states({RW, RO, RO, RO});
    is_secondary();

    // A write will cause a switch to the server with rank=primary
    test.expect(c.query("SELECT @@server_id, @@last_insert_id"), "Query should work");
    is_primary();

    std::cout << "Grouping servers into a three-node cluster with one low-ranking server" << std::endl;

    test.check_maxctrl("alter server server1 rank primary");
    test.check_maxctrl("alter server server2 rank primary");
    test.check_maxctrl("alter server server3 rank primary");
    test.check_maxctrl("alter server server4 rank secondary");

    c.disconnect();
    c.connect();

    set_states({DOWN, RW, RO, RO});

    auto id = c.field("SELECT @@server_id");
    test.expect(!id.empty() && id != ids[3], "Third slave should not reply");

    set_states({DOWN, DOWN, RW, RO});

    id = c.field("SELECT @@server_id");
    test.expect(!id.empty() && id != ids[3], "Third slave should not reply");

    set_states({DOWN, DOWN, DOWN, RW});

    test.expect(c.field("SELECT @@server_id") == ids[3], "Third slave should reply");

    set_states({RO, RO, RO, RW});

    test.expect(c.field("SELECT @@server_id") == ids[3], "Third slave should reply");

    set_states({RW, RO, RO, DOWN});

    id = c.field("SELECT @@server_id");
    test.expect(!id.empty() && id != ids[3], "Third slave should not reply");
}

void test_readconnroute(TestConnections& test, std::vector<std::string> ids)
{
    std::cout << "Readconnroute with descending server rank" << std::endl;

    test.check_maxctrl("alter server server1 rank primary");
    test.check_maxctrl("alter server server2 rank primary");
    test.check_maxctrl("alter server server3 rank secondary");
    test.check_maxctrl("alter server server4 rank secondary");

    auto do_test = [&](int node) {
        Connection c = test.maxscale->readconn_master();
        c.connect();
        test.expect(c.field("SELECT @@server_id") == ids[node], "server%d should reply", node + 1);
    };

    set_states({RW, RO, RO, RO});
    do_test(0);
    set_states({DOWN, RW, RO, RO});
    do_test(1);
    set_states({DOWN, DOWN, RW, RO});
    do_test(2);
    set_states({DOWN, DOWN, DOWN, RW});
    do_test(3);
    set_states({DOWN, DOWN, RW, RO});
    do_test(2);
    set_states({DOWN, RW, RO, RO});
    do_test(1);
    set_states({RW, RO, RO, RO});
    do_test(0);

    std::cout << "MXS-4132: Rank of the first server is ignored with router_options=master" << std::endl;

    test.check_maxctrl("alter service Read-Connection-Router router_options=master");
    test.check_maxctrl("set server server2 master");
    test.check_maxctrl("set server server3 master");
    test.check_maxctrl("set server server4 master");
    test.check_maxctrl("alter server server2 rank secondary");

    do_test(0);
    test.check_maxctrl("clear server server1 master");
    do_test(1);
    test.check_maxctrl("clear server server2 master");
    do_test(2);
    test.check_maxctrl("clear server server3 master");
    do_test(3);

    test.check_maxctrl("alter service Read-Connection-Router router_options=running");
}

void test_hints(TestConnections& test, std::vector<std::string> ids)
{
    std::cout << "Test that routing hints override server rank" << std::endl;

    test.check_maxctrl("alter server server1 rank primary");
    test.check_maxctrl("alter server server2 rank primary");
    test.check_maxctrl("alter server server3 rank primary");
    test.check_maxctrl("alter server server4 rank secondary");

    set_states({RW, RO, RO, RO});

    Connection c = test.maxscale->rwsplit();
    c.connect();

    auto id = c.field("SELECT @@server_id -- maxscale route to server server4");
    test.expect(!id.empty() && id == ids[3], "Third slave should reply");

    id = c.field("SELECT @@server_id -- maxscale route to slave");
    test.expect(!id.empty() && (id == ids[1] || id == ids[2]), "Primary slave should reply");

    id = c.field("SELECT @@server_id -- maxscale route to master");
    test.expect(!id.empty() && id == ids[0], "Master should reply");
}

void test_services(TestConnections& test, std::vector<std::string> ids)
{
    test.log_printf("Test that rank works with services");

    test.check_maxctrl("alter server server1 rank primary");
    test.check_maxctrl("alter server server2 rank primary");
    test.check_maxctrl("alter server server3 rank primary");
    test.check_maxctrl("alter server server4 rank primary");

    set_states({RW, RO, RO, RO});

    Connection c = test.maxscale->get_connection(4009);

    test.check_maxctrl("alter service service1 rank primary");
    test.check_maxctrl("alter service service2 rank secondary");
    test.check_maxctrl("alter service service3 rank secondary");

    // service1 uses server1 and server2
    c.connect();
    test.expect(c.field("SELECT @@server_id") == ids[1], "Second slave should reply");

    test.check_maxctrl("alter service service1 rank secondary");
    test.check_maxctrl("alter service service2 rank primary");
    test.check_maxctrl("alter service service3 rank secondary");

    // service2 uses server1 and server3
    c.connect();
    test.expect(c.field("SELECT @@server_id") == ids[2], "Third slave should reply");

    test.check_maxctrl("alter service service1 rank secondary");
    test.check_maxctrl("alter service service2 rank secondary");
    test.check_maxctrl("alter service service3 rank primary");

    // service3 uses server1 and server4
    c.connect();
    test.expect(c.field("SELECT @@server_id") == ids[3], "Fourth slave should reply");

    // Set all serviecs to the same rank
    test.check_maxctrl("alter service service1 rank secondary");
    test.check_maxctrl("alter service service2 rank secondary");
    test.check_maxctrl("alter service service3 rank secondary");

    c.connect();
    std::set<std::string> id_set(ids.begin() + 1, ids.end());
    test.expect(id_set.count(c.field("SELECT @@server_id")), "Any slave should reply");

    test.check_maxctrl("alter service service1 rank primary");
    test.check_maxctrl("alter service service2 rank primary");
    test.check_maxctrl("alter service service3 rank primary");

    c.connect();
    test.expect(id_set.count(c.field("SELECT @@server_id")), "Any slave should reply");
}

int main(int argc, char* argv[])
{
    TestConnections test(argc, argv);

    set_states = [&](std::vector<std::string> states){
        // Collecting the commands into a file and feeding that into maxctrl speeds up
        // the testing by quite a bit.
        std::ofstream commands("commands.txt");

        for (size_t i = 0; i < states.size(); i++)
        {
            std::string name = "server" + std::to_string(i + 1);
            commands << "clear server server" << (i + 1) << " running master slave maintenance drain\n";
            commands << "set server --force server" << (i + 1) << " " << states[i] << "\n";
        }

        commands.flush();
        test.maxscale->copy_to_node("commands.txt", "/tmp/commands.txt");
        test.maxscale->ssh_node_f(false, "maxctrl --timeout 30s < /tmp/commands.txt");
    };

    test.repl->connect();
    auto ids = test.repl->get_all_server_ids_str();
    test.repl->disconnect();

    test_rwsplit(test, ids);
    test_readconnroute(test, ids);
    test_hints(test, ids);
    test_services(test, ids);

    return test.global_result;
}
