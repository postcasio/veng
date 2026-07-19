#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeId.h>

// A module-defined asset type shared by every side of the seam: the test module registers its
// identity, handle-leaf mapping, and a loader factory through VengModuleRegister, the test cook
// module registers its importer, and loader_test and the cooker suite assert against this one
// spelling.
inline constexpr Veng::AssetTypeId ProbeAssetType{0x82662395A6F8C7DDULL};

// The manifest/authoring name the module registers the type under, and the name a pack entry's
// "type" string must carry to reach the cook module's importer.
inline constexpr const char* ProbeAssetTypeName = "ProbeAsset";

/// @brief The runtime asset a cooked probe blob decodes into: the importer's four bytes, verbatim.
struct ProbeAsset
{
    /// @brief The cooked blob's bytes as the module's loader decoded them.
    Veng::vector<Veng::u8> Bytes;
};

namespace Veng
{
    /// @brief AssetTypeTrait specialization binding ProbeAsset to its minted asset type.
    ///
    /// What lets AssetHandle<ProbeAsset> and AssetManager::LoadSync<ProbeAsset>(id) resolve for a
    /// type the engine has never heard of.
    template <>
    struct AssetTypeTrait<ProbeAsset>
    {
        /// @brief The asset type tag for ProbeAsset.
        static constexpr AssetTypeId Type = ProbeAssetType;
    };
}

// The reflected leaf for the handle field below. A game-defined asset type reaches a reflected
// component only through such a leaf, and the module pairs this id back to ProbeAssetType via
// AssetTypeInfo::HandleFieldType — the registration this fixture exists to exercise.
VE_LEAF(::Veng::AssetHandle<::ProbeAsset>, 0x176D9453E9ABF90AULL, ::Veng::FieldClass::AssetHandle);

// A game-defined component shared by the test module (which registers it through
// VengModuleRegister) and loader_test (which asserts it reflects correctly in the
// host registry). Defined once so both sides agree on its TypeId and field shape.
//
// Its Asset field is the whole point: a component referencing a module-defined asset type the
// way a game authors one, so the prefab dependency path is covered against a non-builtin type.
struct Probe
{
    Veng::f32 Value = 1.0f;
    Veng::AssetHandle<ProbeAsset> Asset;
};

VE_REFLECT(::Probe, 0x7E5701A2B3C4D5E6ULL)
VE_FIELD(Value, .DisplayName = "Probe Value")
VE_FIELD(Asset, .DisplayName = "Probe Asset")
VE_REFLECT_END();
