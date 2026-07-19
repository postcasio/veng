#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/AssetType.h>

/// @brief A game-defined asset type end to end: identity, cooked layout, runtime class, loader.
///
/// The engine defines none of this. The template mints the type id, registers it and a loader
/// factory through VengModuleRegister, and cooks its JSON source through an importer its cook
/// module contributes — the whole custom-asset seam in one small, self-contained type. This
/// header is the contract the two halves share: the cook module writes the layout below, the
/// runtime loader reads it, and neither can drift without the other failing to compile.
namespace Template
{
    /// @brief The minted identity of this consumer-defined asset type.
    ///
    /// Minted with `vengc generate-asset-type --module <lib>`, which collision-checks against the
    /// engine builtins and every type the named module already registers.
    inline constexpr Veng::AssetTypeId MarkerSetAssetType{0xCE3FFFC917EF1C7FULL};

    /// @brief The canonical name a pack manifest's `"type"` string carries for this type.
    inline constexpr const char* MarkerSetTypeName = "MarkerSet";

    /// @brief Fixed-size header leading a cooked marker-set blob.
    struct CookedMarkerSetHeader
    {
        /// @brief Format tag identifying the blob; a mismatch is a load error.
        Veng::u32 Magic;
        /// @brief Layout version the loader checks; a mismatch is a load error.
        Veng::u32 Version;
        /// @brief Number of MarkerRecord entries following this header.
        Veng::u32 MarkerCount;
        /// @brief Byte length of the name heap following the records.
        Veng::u32 NameHeapBytes;
    };

    /// @brief One marker's fixed-stride record; its name is a slice of the trailing name heap.
    struct CookedMarkerRecord
    {
        /// @brief The marker's position, x/y/z.
        Veng::f32 Position[3];
        /// @brief Byte offset of the marker's name within the name heap.
        Veng::u32 NameOffset;
        /// @brief Byte length of the marker's name.
        Veng::u32 NameLength;
    };

    /// @brief Blob format tag ("MRKS"), written by the importer and checked by the loader.
    inline constexpr Veng::u32 MarkerSetMagic = 0x534B524DU;

    /// @brief Cooked layout version; the loader rejects any other value.
    inline constexpr Veng::u32 MarkerSetVersion = 1;

    /// @brief One decoded marker: a name and a position.
    struct Marker
    {
        /// @brief The marker's authored name.
        Veng::string Name;
        /// @brief The marker's position in level space.
        Veng::vec3 Position{0.0f};
    };

    /// @brief The runtime asset a cooked marker-set blob decodes into.
    ///
    /// Carries no GPU resource, so its loader needs neither the render context nor an upload —
    /// the simplest shape a consumer-defined asset takes.
    struct MarkerSet
    {
        /// @brief Every marker the set declares, in authored order.
        Veng::vector<Marker> Markers;

        /// @brief Returns the marker with the given name, or nullptr when the set declares none.
        /// @param name  The marker name to look up.
        /// @return The matching marker, or nullptr.
        [[nodiscard]] const Marker* Find(Veng::string_view name) const;
    };

    /// @brief Decodes cooked marker-set blobs; registered as a factory through VengModuleRegister.
    class MarkerSetLoader final : public Veng::AssetLoader
    {
    public:
        /// @brief Returns MarkerSetAssetType, the type this loader is dispatched for.
        [[nodiscard]] Veng::AssetTypeId Type() const override { return MarkerSetAssetType; }

        /// @brief Decodes the blob into a MarkerSet; needs no dependency load and no finalize.
        /// @param id      The asset being loaded, named in a decode error.
        /// @param cooked  The cooked blob bytes from the archive.
        /// @return The decoded set, or a structured load error on a malformed blob.
        [[nodiscard]] Veng::AssetResult<Veng::Detail::LoadJob>
        Load(Veng::AssetManager& manager, Veng::Renderer::Context& context, Veng::TaskSystem& tasks,
             Veng::TypeRegistry& types, Veng::AssetId id, std::span<const Veng::u8> cooked,
             bool async) const override;
    };
}

namespace Veng
{
    /// @brief AssetTypeTrait specialization binding MarkerSet to its minted asset type.
    ///
    /// What makes AssetHandle<MarkerSet> and AssetManager::Load<MarkerSet>(id) resolve — a
    /// consumer-defined type reaches the ordinary typed load path through this one specialization.
    template <>
    struct AssetTypeTrait<Template::MarkerSet>
    {
        /// @brief The asset type tag for MarkerSet.
        static constexpr AssetTypeId Type = Template::MarkerSetAssetType;
    };
}
