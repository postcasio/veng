// doctest entry point for the veng unit suite.
//
// This TU defines main(); each unit test file contributes its own *.cpp of
// TEST_CASEs that link into the `veng_unit` executable alongside it.
// Keep this file free of test cases so the single IMPLEMENT_WITH_MAIN macro
// has exactly one home.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
