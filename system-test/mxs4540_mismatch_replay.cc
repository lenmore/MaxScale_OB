/*
 * Copyright (c) 2022 MariaDB Corporation Ab
 * Copyright (c) 2023 MariaDB plc, Finnish Branch
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-06-03
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <maxtest/testconnections.hh>

void test_main(TestConnections& test)
{
    auto* admin = test.repl->backend(0)->admin_connection();
    auto user = admin->create_user("bob", "%", "bob");
    user.grant("ALL ON *.*");
    auto c = test.maxscale->rwsplit();
    c.set_credentials("bob", "bob");

    if (test.expect(c.connect()
                    && c.query("START TRANSACTION")
                    && c.query("SELECT UUID()"),
                    "Failed to start transaction: %s", c.error()))
    {
        admin->cmd("KILL USER bob");

        // The replay limit should eventually cause the replay to fail
        test.expect(!c.query("COMMIT"), "The transaction should fail to commit after replay");
    }
}

int main(int argc, char** argv)
{
    return TestConnections{}.run_test(argc, argv, test_main);
}
