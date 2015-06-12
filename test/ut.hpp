/*
 * ut.hpp
 *
 *  Created on: 2015��5��21��
 *      Author: wangqiying
 */

#ifndef TEST_UT_HPP_
#define TEST_UT_HPP_

#include <stdio.h>
#include <string>
#include <map>
#include <vector>
#include <set>
#include <strstream>
#include "mmkv.hpp"
#include "types.hpp"

// Expands to the name of the class that implements the given test.
#define GTEST_TEST_CLASS_NAME_(test_case_name, test_name) \
  test_case_name##_##test_name##_Test

#define GTEST_TEST_(test_case_name, test_name)\
class GTEST_TEST_CLASS_NAME_(test_case_name, test_name) : public mmkv::TestCase {\
 public:\
  GTEST_TEST_CLASS_NAME_(test_case_name, test_name)() {mmkv::GetGlobalRegister()->RegisterTestCase(#test_case_name, #test_name, this);}\
 private:\
  virtual void TestBody();\
  const char* TestName() { return #test_name;}\
  const char* TestCaseName() { return #test_case_name;}\
  static GTEST_TEST_CLASS_NAME_(test_case_name, test_name)* const singleton;\
};\
\
GTEST_TEST_CLASS_NAME_(test_case_name, test_name)* const GTEST_TEST_CLASS_NAME_(test_case_name, test_name)::singleton = new GTEST_TEST_CLASS_NAME_(test_case_name, test_name);\
void GTEST_TEST_CLASS_NAME_(test_case_name, test_name)::TestBody()

#define TEST(test_case_name, test_name)\
  GTEST_TEST_(test_case_name, test_name)


#define FORCE_CORE_DUMP() do {int *p = NULL; *p=0;} while(0)

/* Evaluates to the same boolean value as 'p', and hints to the compiler that
 * we expect this value to be false. */
#ifdef __GNUC__
#define __UNLIKELY(p) __builtin_expect(!!(p),0)
#else
#define __UNLIKELY(p) (p)
#endif

#define ABORT(msg) \
    do {                                \
            (void)fprintf(stderr,  \
                "%s:%d: Failed %s ",     \
                __FILE__,__LINE__,msg);      \
            abort();                    \
    } while (0)

#define ASSERT(cond)                     \
    do {                                \
        if (__UNLIKELY(!(cond))) {             \
            /* In case a user-supplied handler tries to */  \
            /* return control to us, log and abort here. */ \
            (void)fprintf(stderr,               \
                "%s:%d: Assertion %s failed in %s",     \
                __FILE__,__LINE__,#cond,__func__);      \
            abort();                    \
        }                           \
    } while (0)

#define ALLOC_ASSERT(x) \
    do {\
        if (__UNLIKELY (!x)) {\
            fprintf (stderr, "FATAL ERROR: OUT OF MEMORY (%s:%d)\n",\
                __FILE__, __LINE__);\
            abort();\
        }\
    } while (false)

#define CHECK_FATAL(cond, ...)  do{\
    if(cond){\
         (void)fprintf(stderr,               \
                        "\e[1;35m%-6s\e[m%s:%d: Assertion %s failed in %s\n",     \
                        "[FAIL]", __FILE__,__LINE__,#cond,__func__);      \
         fprintf(stderr, "\e[1;35m%-6s\e[m", "[FAIL]:"); \
         fprintf(stderr, __VA_ARGS__);\
         fprintf(stderr, "\n"); \
         exit(-1);\
    }else{\
    }\
}while(0)

#define CHECK_CMP(TYPE, cond1, cond2, cmp, ...)  do{\
    TYPE cond1_v  = cond1; TYPE cond2_v = cond2; \
    if(!(cond1_v cmp cond2_v)){\
         (void)fprintf(stderr,               \
                        "\e[1;35m%-6s\e[m%s:%d: Assertion %s %s %s failed in %s\n",     \
                        "[FAIL]", __FILE__,__LINE__,#cond1, #cmp, #cond2, __func__);      \
         std::strstream ss; \
         ss << #cond1 << " == " << cond1_v << " NOT " << #cmp <<" value:" << cond2; \
         fprintf(stderr, "\e[1;35m%-6s\e[m", "[FAIL]:"); \
         fprintf(stderr, "%s ", ss.str()); \
         fprintf(stderr, __VA_ARGS__);\
         fprintf(stderr, "\n"); \
         exit(-1);\
    }else{\
    }\
}while(0)

#define CHECK_EQ(TYPE, cond1, cond2, ...) CHECK_CMP(TYPE, cond1, cond2, ==, __VA_ARGS__)

namespace mmkv
{
    class TestCase
    {
        public:
            virtual void TestBody() = 0;
            virtual const char* TestName() = 0;
            virtual const char* TestCaseName() = 0;
            virtual ~TestCase()
            {
            }
    };

    class Register
    {
        private:
            typedef std::vector<TestCase*> TestCaseArray;
            TestCaseArray m_all_tests;
        public:
            void RegisterTestCase(const std::string& test_case_name, const std::string& test_name, TestCase* test_case);
            void RunAll();
    };
    void RunAllTests();
    Register* GetGlobalRegister();

}
extern mmkv::MMKV* g_test_kv;

#endif /* TEST_UT_HPP_ */