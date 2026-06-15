#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>

// A game-defined component shared by the test module (which registers it through
// VengModuleRegister) and loader_test (which asserts it reflects correctly in the
// host registry). Defined once so both sides agree on its TypeId and field shape.
struct Probe
{
    Veng::f32 Value = 1.0f;
};

VE_REFLECT(Probe, 0x7E5701A2B3C4D5E6ULL)
    VE_FIELD(Value, .DisplayName = "Probe Value")
VE_REFLECT_END();
