// doctest entry point for the veng unit suite (planset-3, plan 01).
//
// This TU defines main(); each test band (plans 02–04) contributes its own
// *.cpp of TEST_CASEs that link into the `veng_unit` executable alongside it.
// Keep this file free of test cases so the single IMPLEMENT_WITH_MAIN macro
// has exactly one home.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
