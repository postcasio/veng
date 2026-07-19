#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetLoader.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Reflection/TypeId.h>

/// @brief Export annotation for the out-of-line symbols libtemplate shares with libtemplate_cook.
///
/// The cook module links the runtime library, so an importer may call code the runtime defines —
/// but only if that symbol is importable across the two images. On macOS/Linux default visibility
/// makes it so; on Windows a symbol must be `dllexport`ed by the defining DLL and `dllimport`ed
/// by the consumer, and veng's own VE_MODULE_EXPORT is unconditionally `dllexport` (it annotates
/// the module's own C-ABI entry, which is never imported). So a game that shares out-of-line code
/// between its two images needs its own two-sided macro; CMake defines `<target>_EXPORTS` while
/// compiling the target itself, which is the side that must export.
#if defined(_WIN32)
#if defined(template_EXPORTS)
#define TEMPLATE_API __declspec(dllexport)
#else
#define TEMPLATE_API __declspec(dllimport)
#endif
#else
#define TEMPLATE_API __attribute__((visibility("default")))
#endif

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

    /// @brief Folds an authored marker name to the single spelling both halves of the type agree on.
    ///
    /// Trims surrounding whitespace and lowercases, so a source file may author "Overlook " and a
    /// lookup may ask for "overlook". Deliberately out of line and defined in the runtime library:
    /// the importer calls it while writing the name heap and MarkerSet::Find calls it while
    /// reading, so the two images cannot disagree about what a name means — and a game sharing
    /// real code (not just layouts) across the seam is exactly what COOK_SOURCES' link makes
    /// possible.
    /// @param name  The authored name.
    /// @return The folded spelling.
    [[nodiscard]] TEMPLATE_API Veng::string NormalizeMarkerName(Veng::string_view name);

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

// The reflected leaf for AssetHandle<MarkerSet>, minted with `vengc generate-type-id`. Without it
// the handle cannot appear on a component at all; with it — and with the matching
// AssetTypeInfo::HandleFieldType the module registers — a prefab authors a reference to a
// consumer-defined asset exactly as it authors one to a builtin.
VE_LEAF(::Veng::AssetHandle<::Template::MarkerSet>, 0x45F12CEFFB288CDDULL,
        ::Veng::FieldClass::AssetHandle);
