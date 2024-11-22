/*
 * Copyright (c) 2016 MariaDB Corporation Ab
 * Copyright (c) 2023 MariaDB plc, Finnish Branch
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2026-03-20
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include <maxbase/string.hh>
#include <maxbase/assert.hh>
#include <string.h>
#include <iostream>
#include <tuple>
#include <numeric>
#include <random>

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

int test_split()
{
    cout << "split()" << endl;

    std::vector<std::tuple<std::string_view,
                           std::string_view,
                           std::string_view,
                           std::string_view>> test_cases
    {
        {"hello=world", "=", "hello", "world", },
        {"=world", "=", "", "world", },
        {"=world", "", "=world", ""},
        {"helloworld!", "!", "helloworld", ""},
        {"helloworld!", "=", "helloworld!", ""},
        {"helloworld!", "\0", "helloworld!", ""},
        {"hello world!", "  ", "hello world!", ""},
        {"hello world!", " ", "hello", "world!"},
        {"hello world!", "world", "hello ", "!"},
    };

    int rc = 0;

    for (const auto& [input, delim, head, tail] : test_cases)
    {
        if (auto [split_head, split_tail] =
                mxb::split(input, delim); head != split_head || tail != split_tail)
        {
            cout << "`" << input << "` with delimiter `" << delim << "` returned "
                 << "`" << split_head << "` and `" << split_tail << "` "
                 << "instead of `" << head << "` and `" << tail << "`" << endl;
            rc = 1;
        }
    }

    return rc;
}

int test_cat()
{
    cout << "cat()" << endl;
    int rc = 0;

    auto expect = [&](std::string result, auto expected){
        if (result != expected)
        {
            cout << "Expected '" << expected << "' got '" << result << "'";
            rc++;
        }
    };

    expect(mxb::cat("", ""), "");
    expect(mxb::cat("1"), "1");
    expect(mxb::cat("2", ""), "2");
    expect(mxb::cat("", "3"), "3");
    expect(mxb::cat("", "4", ""), "4");

    expect(mxb::cat("hello", "world"), "helloworld");
    expect(mxb::cat(std::string("hello"), "world"), "helloworld");
    expect(mxb::cat(std::string_view("hello"), "world"), "helloworld");

    expect(mxb::cat("hello", "world"), "helloworld");
    expect(mxb::cat("hello", std::string("world")), "helloworld");
    expect(mxb::cat("hello", std::string_view("world")), "helloworld");

    expect(mxb::cat(std::string_view("hello"), "world"), "helloworld");
    expect(mxb::cat(std::string_view("hello"), std::string("world")), "helloworld");
    expect(mxb::cat(std::string_view("hello"), std::string_view("world")), "helloworld");

    std::string str = "std::string";
    std::string_view sv = "std::string_view";
    const char* cchar = "const char*";

    expect(mxb::cat(str), str);
    expect(mxb::cat(sv), sv);
    expect(mxb::cat(cchar), cchar);

    expect(mxb::cat(str, sv), str + std::string {sv});
    expect(mxb::cat(str, cchar), str + cchar);
    expect(mxb::cat(sv, str), std::string {sv} + str);
    expect(mxb::cat(sv, cchar), std::string {sv} + cchar);
    expect(mxb::cat(cchar, str), cchar + str);
    expect(mxb::cat(cchar, sv), cchar + std::string {sv});

    return rc;
}

// This is the old version of mxb::strtok. Here only as a safeguard against unexpected changes.
inline std::vector<std::string> strtok_old(std::string str, const char* delim)
{
    std::vector<std::string> rval;
    char* save_ptr;
    char* tok = strtok_r(&str[0], delim, &save_ptr);

    while (tok)
    {
        rval.emplace_back(tok);
        tok = strtok_r(NULL, delim, &save_ptr);
    }

    return rval;
}

template<class Func>
int test_strtok(Func func, const char* func_name)
{
    cout << func_name << "()" << endl;

    std::vector<std::tuple<const char*, const char*,
                           std::vector<std::string>>> test_cases
    {
        {"hello=world", "=", {"hello", "world"}},
        {"=world", "=", {"world"}},
        {"=world", "", {"=world"}},
        {"helloworld!", "!", {"helloworld"}},
        {"helloworld!", "=", {"helloworld!"}},
        {"helloworld!", "\0", {"helloworld!"}},
        {"hello world!", "  ", {"hello", "world!"}},
        {"hello world!", " ", {"hello", "world!"}},
        {"hello world!", "world", {"he", " ", "!"}},
        {"!hello world!", "!", {"hello world"}},

        {"server1, server2, server3, server4", ", ",
         {"server1", "server2", "server3", "server4"}},

        {"https://en.cppreference.com/w/cpp/string/basic_string/find", "/",
         {"https:", "en.cppreference.com", "w", "cpp", "string", "basic_string", "find"}},
    };

    int rc = 0;

    for (const auto& [input, delim, expected] : test_cases)
    {
        auto result = func(input, delim);

        if (result != expected)
        {
            cout << "`" << input << "` with delimiter `" << delim << "` returned "
                 << mxb::join(result, ", ", "`") << " instead of " << mxb::join(expected, ", ", "`") << endl;
            rc = 1;
        }
    }
    return rc;
}

int compare_old_strtok()
{
    cout << "comparing new strtok() to old strtok()" << endl;
    int rc = 0;
    std::string input;

    for (uint8_t i = 0; i < std::numeric_limits<uint8_t>::max(); i++)
    {
        if (isprint(i) || isspace(i))
        {
            input.push_back(i);
        }
    }

    // Using a static seed makes the test deterministic.
    std::mt19937 engine(1234);

    for (int i = 0; i < 100000; i++)
    {
        // Take the first five characters of the previous string as delimiters
        // and then shuffle the string.
        std::string delim = input.substr(0, 5);
        std::shuffle(input.begin(), input.end(), engine);

        auto result = mxb::strtok(input, delim);
        auto expected = strtok_old(input, delim.c_str());

        if (result != expected)
        {
            cout << "`" << input << "` with delimiter `" << delim << "` returned "
                 << mxb::join(result, ", ", "`") << " instead of " << mxb::join(expected, ", ", "`") << endl;
            rc = 1;
            break;
        }
    }

    return rc;
}
}

int main(int argc, char* argv[])
{
    int rv = 0;

    rv += test_trim();
    rv += test_ltrim();
    rv += test_rtrim();
    rv += test_split();
    rv += test_cat();
    rv += test_strtok(strtok_old, "strtok_old");
    rv += test_strtok(mxb::strtok, "strtok");
    rv += compare_old_strtok();

    return rv;
}
