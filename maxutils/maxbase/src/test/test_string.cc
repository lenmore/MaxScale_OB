/*
 * Copyright (c) 2016 MariaDB Corporation Ab
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

#include <maxbase/string.hh>
#include <maxbase/assert.h>
#include <string.h>
#include <iostream>

using std::cout;
using std::endl;

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

namespace
{

#define TRIM_TCE(zFrom, zTo) {zFrom, zTo}

struct TRIM_TEST_CASE
{
    const char* zFrom;
    const char* zTo;
};

TRIM_TEST_CASE trim_testcases[] =
{
    TRIM_TCE("",        ""),
    TRIM_TCE("a",       "a"),
    TRIM_TCE(" a",      "a"),
    TRIM_TCE("a ",      "a"),
    TRIM_TCE(" a ",     "a"),
    TRIM_TCE("  a",     "a"),
    TRIM_TCE("a  ",     "a"),
    TRIM_TCE("  a  ",   "a"),
    TRIM_TCE("  a b  ", "a b"),
};

const int n_trim_testcases = sizeof(trim_testcases) / sizeof(trim_testcases[0]);

TRIM_TEST_CASE ltrim_testcases[] =
{
    TRIM_TCE("",        ""),
    TRIM_TCE("a",       "a"),
    TRIM_TCE(" a",      "a"),
    TRIM_TCE("a ",      "a "),
    TRIM_TCE(" a ",     "a "),
    TRIM_TCE("  a",     "a"),
    TRIM_TCE("a  ",     "a  "),
    TRIM_TCE("  a  ",   "a  "),
    TRIM_TCE("  a b  ", "a b  "),
};

const int n_ltrim_testcases = sizeof(ltrim_testcases) / sizeof(ltrim_testcases[0]);

TRIM_TEST_CASE rtrim_testcases[] =
{
    TRIM_TCE("",        ""),
    TRIM_TCE("a",       "a"),
    TRIM_TCE(" a",      " a"),
    TRIM_TCE("a ",      "a"),
    TRIM_TCE(" a ",     " a"),
    TRIM_TCE("  a",     "  a"),
    TRIM_TCE("a  ",     "a"),
    TRIM_TCE("  a  ",   "  a"),
    TRIM_TCE("  a b  ", "  a b"),
};

const int n_rtrim_testcases = sizeof(rtrim_testcases) / sizeof(rtrim_testcases[0]);


int test(TRIM_TEST_CASE* pTest_cases, int n_test_cases, char* (*p)(char*))
{
    int rv = 0;

    for (int i = 0; i < n_test_cases; ++i)
    {
        const char* zFrom = pTest_cases[i].zFrom;
        const char* zTo = pTest_cases[i].zTo;

        char copy[strlen(zFrom) + 1];
        strcpy(copy, zFrom);

        char* z = p(copy);

        if (strcmp(z, zTo) != 0)
        {
            ++rv;
        }
    }

    return rv;
}

int test_trim()
{
    cout << "trim()" << endl;
    return test(trim_testcases, n_trim_testcases, mxb::trim);
}

int test_ltrim()
{
    cout << "ltrim()" << endl;
    return test(ltrim_testcases, n_ltrim_testcases, mxb::ltrim);
}

int test_rtrim()
{
    cout << "rtrim()" << endl;
    return test(rtrim_testcases, n_rtrim_testcases, mxb::rtrim);
}
}

int main(int argc, char* argv[])
{
    int rv = 0;

    rv += test_trim();
    rv += test_ltrim();
    rv += test_rtrim();

    return rv;
}
