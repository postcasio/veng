#pragma once

#include <Veng/Reflection/Reflect.h>
#include <Veng/Veng.h>

// tests/support/TestComponents.h — test-local reflected components the suites register.
//
// The replication, interest, state-flow, and reconciliation suites need a pre-registered
// replicated guinea pig that is independent of any gameplay builtin. TestScore is that component:
// a one-field replicated (and always-relevant, for the interest exemption cases) value a fixture
// registers into its own TypeRegistry beside the builtins. TestHealth is its server-owned sibling:
// replicated state a predicting peer takes as authoritative and never reconciles.

namespace VengTest
{
    /// @brief A test-local replicated component: discrete state the suites author and assert on.
    struct TestScore
    {
        /// @brief The replicated value.
        Veng::i32 Value = 0;
    };

    /// @brief A test-local server-owned component: replicated state only the server writes.
    struct TestHealth
    {
        /// @brief The server-written value.
        Veng::i32 Value = 0;
    };
}

VE_REFLECT(::VengTest::TestScore, 0x5415808682D2C1D7ULL)
VE_FIELD(Value, .DisplayName = "Value")
VE_REFLECT_END();
VE_REPLICATED(::VengTest::TestScore);
VE_ALWAYS_RELEVANT(::VengTest::TestScore);

VE_REFLECT(::VengTest::TestHealth, 0x4A080B46A162BC8EULL)
VE_FIELD(Value, .DisplayName = "Value")
VE_REFLECT_END();
VE_REPLICATED(::VengTest::TestHealth);
VE_SERVER_OWNED(::VengTest::TestHealth);
