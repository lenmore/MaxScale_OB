/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 * Copyright (c) 2023 MariaDB plc, Finnish Branch
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

void different_packet_size(TestConnections& test)
{
    test.repl->execute_query_all_nodes("SET GLOBAL max_allowed_packet=1073741824");

    std::vector<std::thread> threads;
    std::string prefix = "SELECT '";
    std::string suffix = "' AS value";
    const int loops = 3;
    const int range = 2;

    auto done = [&](size_t sz){
        static std::mutex lock;
        std::lock_guard guard(lock);
        test.tprintf("Done: %lu", sz);
    };

    for (int i = 1; i <= loops; i++)
    {
        for (int j = -range; j <= range; j++)
        {
            size_t size = 0x0ffffff * i + j;
            threads.emplace_back([&, size](){
                size_t constant_size = size - prefix.size() - suffix.size() - 1;
                std::string sql = prefix;
                sql.append(constant_size, 'a');
                sql.append(suffix);

                auto c = test.maxscale->rwsplit();
                test.expect(c.connect(), "Failed to connect: %s", c.error());
                test.expect(c.query(sql), "Query with size %lu failed: %s", sql.size(), c.error());
                done(sql.size());
            });
        }
    }

    for (auto& t : threads)
    {
        t.join();
    }

    test.repl->execute_query_all_nodes("SET GLOBAL max_allowed_packet=DEFAULT");
}

int main(int argc, char** argv)
{
    return TestConnections().run_test(argc, argv, different_packet_size);
}
