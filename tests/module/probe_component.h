#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Reflection/Reflect.h>

// A game-defined component shared by the test module (which registers it through
// VengModuleRegister) and loader_test (which asserts it reflects correctly in the
// host registry). Defined once so both sides agree on its TypeId and field shape.
struct Probe
{
    Veng::f32 Value = 1.0f;
};

VE_REFLECT(::Probe, 0x7E5701A2B3C4D5E6ULL)
VE_FIELD(Value, .DisplayName = "Probe Value")
VE_REFLECT_END();

// A module-defined asset type shared the same way: the test module registers its identity and a
// loader factory through VengModuleRegister, the test cook module registers its importer, and
// both loader_test and the cooker suite assert against this one spelling.
inline constexpr Veng::AssetTypeId ProbeAssetType{0x82662395A6F8C7DDULL};

// The manifest/authoring name the module registers the type under, and the name a pack entry's
// "type" string must carry to reach the cook module's importer.
inline constexpr const char* ProbeAssetTypeName = "ProbeAsset";
