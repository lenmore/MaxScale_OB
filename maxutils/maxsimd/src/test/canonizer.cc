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

#include <maxscale/ccdefs.hh>

#include <string.h>
#include <fstream>
#include <iostream>
#include <string>

#include <maxsimd/canonical.hh>

#ifdef __aarch64__
extern "C" const char* __asan_default_options()
{
    // For some reason this is extremely slow on a few Ubuntu distributions on aarch64
    // if ASAN is detecting stack-use-after-return. This is potentially one of these bugs:
    // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=91101
    // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=94910
    return "detect_stack_use_after_return=false";
}
#endif

using std::cout;
using std::endl;

int main(int argc, char** argv)
{
    int rc = EXIT_FAILURE;

    if (argc != 3)
    {
        cout << "Usage: canonizer <input file> <output file>" << endl;
        return rc;
    }

    std::ifstream infile(argv[1]);
    std::ofstream outfile(argv[2]);

    if (infile && outfile)
    {
        for (std::string line; getline(infile, line);)
        {
            while (*line.rbegin() == '\n')
            {
                line.resize(line.size() - 1);
            }

            if (!line.empty())
            {
                maxsimd::get_canonical(&line);
                outfile << line << endl;
            }
        }

        rc = EXIT_SUCCESS;
    }
    else
    {
        cout << "Opening files failed." << endl;
    }

    return rc;
}
