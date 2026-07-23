#pragma once

#include <Veng/Asset/Types.h>

#include <compare>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>

namespace Veng
{
    /// @brief Opaque 64-bit asset-type identifier — the AssetId discipline applied to asset types.
    ///
    /// Engine builtins carry a hardcoded 0x…ULL literal checked into the source (see the
    /// AssetTypes namespace below); anything else mints its own with `vengc generate-asset-type`.
    /// 0 is the reserved invalid id. A separate space from the reflection TypeId: not every asset
    /// type has a reflected runtime struct, and assetpack carries no reflection dependency.
    struct AssetTypeId
    {
        /// @brief The raw identifier value; 0 is invalid.
        u64 Value = 0;

        /// @brief Returns true if the id is non-zero (i.e. not the reserved invalid id).
        [[nodiscard]] bool IsValid() const { return Value != 0; }

        /// @brief Three-way comparison, giving AssetTypeId a total order for sorted containers.
        auto operator<=>(const AssetTypeId&) const = default;
    };
}

/// @brief std::hash specialization for Veng::AssetTypeId, enabling use in unordered containers.
template <>
struct std::hash<Veng::AssetTypeId>
{
    /// @brief Returns the hash of the AssetTypeId's underlying value.
    /// @param id  The id to hash.
    /// @return Hash of id.Value.
    Veng::usize operator()(const Veng::AssetTypeId& id) const noexcept
    {
        return std::hash<Veng::u64>{}(id.Value);
    }
};

namespace Veng
{
    /// @brief The minted identities of the asset types the engine itself defines.
    namespace AssetTypes
    {
        /// @brief Untyped raw blob; consumed as-is by the engine.
        inline constexpr AssetTypeId Raw{0x1DCB266645D40BD6ULL};
        /// @brief A decoded texture with sampler parameters (see CookedTextureHeader).
        inline constexpr AssetTypeId Texture{0x1AC836A05E01BEABULL};
        /// @brief An interleaved vertex + index mesh (see CookedMeshHeader).
        inline constexpr AssetTypeId Mesh{0x96E37F1AC2F2B897ULL};
        /// @brief A SPIR-V module with reflected interface (see CookedShaderHeader).
        inline constexpr AssetTypeId Shader{0xC3F6E9945981B1C3ULL};
        /// @brief A bindless material referencing vertex and fragment shader assets (see CookedMaterialHeader).
        inline constexpr AssetTypeId Material{0x0DBF18D37AA81057ULL};
        /// @brief A parameter override over a parent Material (see CookedMaterialInstanceHeader).
        inline constexpr AssetTypeId MaterialInstance{0x15D3ABED3BF10FDFULL};
        /// @brief A named list of vertex-buffer elements referenced by shaders (see CookedVertexLayoutHeader).
        inline constexpr AssetTypeId VertexLayout{0x295A1E72305EE114ULL};
        /// @brief A tree of entities with components, spawnable into a Scene (see CookedPrefabHeader).
        inline constexpr AssetTypeId Prefab{0x09ACF730AACBB93CULL};
        /// @brief A world prefab plus level-scoped wiring (game mode, system set, render settings) (see CookedLevelHeader).
        inline constexpr AssetTypeId Level{0x3E2195B59E24EEA5ULL};
        /// @brief A bone hierarchy with inverse-bind matrices for skinning (see CookedSkeletonHeader).
        inline constexpr AssetTypeId Skeleton{0x2BFF065B97CC3601ULL};
        /// @brief A set of per-bone keyframe tracks animating a skeleton (see CookedAnimationHeader).
        inline constexpr AssetTypeId Animation{0x4EF01CC24F48BBB8ULL};
        /// @brief An equirectangular HDR environment map for image-based lighting (see CookedEnvironmentHeader).
        inline constexpr AssetTypeId Environment{0x4993763F8B281317ULL};
        /// @brief A named set of input-action declarations and raw-source bindings (see CookedInputMapHeader).
        inline constexpr AssetTypeId InputMap{0x821332E5B6BE4BF1ULL};
        /// @brief An MSDF glyph atlas plus per-glyph and kerning metrics (see CookedFontHeader).
        inline constexpr AssetTypeId Font{0xBEE6C517FB64FC53ULL};
        /// @brief A flattened, resolved set of USS-like style rules keyed by selector and state (see CookedStyleSheetHeader).
        inline constexpr AssetTypeId StyleSheet{0xF1DBE163FAA8AA8DULL};
        /// @brief A binary UI element tree referencing its fonts, textures, and stylesheets (see CookedUIDocumentHeader).
        inline constexpr AssetTypeId UIDocument{0xD73125F4E6330F4CULL};
        /// @brief A typed column declaration with a key column, describing a DataTable's rows (see CookedTableSchemaHeader).
        inline constexpr AssetTypeId TableSchema{0xC1B0EAF8E201936DULL};
        /// @brief Rows of structured data cooked and validated against a TableSchema (see CookedDataTableHeader).
        inline constexpr AssetTypeId DataTable{0x29EAA6FA75196517ULL};
    }

    /// @brief The reflection TypeIds of the AssetHandle\<T\> leaves that reference a builtin type.
    ///
    /// The value half of AssetTypeInfo::HandleFieldType for the engine's own types. They are
    /// reflection ids, but a reflection TypeId is a plain u64 alias, so recording them here costs
    /// assetpack no dependency — and it is what lets one call to RegisterBuiltinAssetTypes give
    /// every host, including the veng-free core-pack bootstrap, a complete registry.
    ///
    /// Each is the literal authored on the matching VE_LEAF in Veng/Reflection/TypeId.h;
    /// Veng/Asset/AssetHandleType.h static_asserts the two spellings agree.
    namespace AssetHandleFieldTypes
    {
        /// @brief TypeId of AssetHandle\<RawAsset\>.
        inline constexpr u64 Raw = 0x05A5061C9E34F8D3ULL;
        /// @brief TypeId of AssetHandle\<Texture\>.
        inline constexpr u64 Texture = 0x612EE7E69BE7B848ULL;
        /// @brief TypeId of AssetHandle\<Mesh\>.
        inline constexpr u64 Mesh = 0x1CD2C85C50AFC9E0ULL;
        /// @brief TypeId of AssetHandle\<Material\>.
        inline constexpr u64 Material = 0x3992D11EB4362B4CULL;
        /// @brief TypeId of AssetHandle\<MaterialInstance\>.
        inline constexpr u64 MaterialInstance = 0xB47397CC23B08FDEULL;
        /// @brief TypeId of AssetHandle\<Prefab\>.
        inline constexpr u64 Prefab = 0xF71230AEA9060D83ULL;
        /// @brief TypeId of AssetHandle\<Level\>.
        inline constexpr u64 Level = 0xED75AEF99D36E43BULL;
        /// @brief TypeId of AssetHandle\<Skeleton\>.
        inline constexpr u64 Skeleton = 0xCF758350A84A3CE1ULL;
        /// @brief TypeId of AssetHandle\<Animation\>.
        inline constexpr u64 Animation = 0xED6B03478BD050CEULL;
        /// @brief TypeId of AssetHandle\<EnvironmentMap\>.
        inline constexpr u64 Environment = 0x4E2499935571083DULL;
        /// @brief TypeId of AssetHandle\<InputMappingContext\>.
        inline constexpr u64 InputMap = 0xA6CA03617AA27317ULL;
        /// @brief TypeId of AssetHandle\<Font\>.
        inline constexpr u64 Font = 0x1FE6D744331DABE7ULL;
        /// @brief TypeId of AssetHandle\<Gui::StyleSheet\>.
        inline constexpr u64 StyleSheet = 0x29CA592C9355A65AULL;
        /// @brief TypeId of AssetHandle\<Gui::UIDocument\>.
        inline constexpr u64 UIDocument = 0xC591D0D0452797E1ULL;
        /// @brief TypeId of AssetHandle\<TableSchema\>.
        inline constexpr u64 TableSchema = 0x2879BAC5F35A3945ULL;
        /// @brief TypeId of AssetHandle\<DataTable\>.
        inline constexpr u64 DataTable = 0xCC431A7163938F1DULL;
    }

    /// @brief What a registry records about one asset type.
    struct AssetTypeInfo
    {
        /// @brief The type's minted identity.
        AssetTypeId Id;
        /// @brief Canonical authoring/manifest name ("Texture", "MaterialInstance", …).
        ///
        /// The single spelling a pack manifest's `"type"` string and every tool agree on.
        string Name;
        /// @brief Human-readable name for editor display; falls back to Name when empty.
        string DisplayName;
        /// @brief Short badge glyph for editor display ("TEX", "MSH", …).
        string Glyph;
        /// @brief Reflection TypeId of the AssetHandle\<T\> leaf referencing this type; 0 when none.
        ///
        /// What makes an `AssetHandle<T>` field on a reflected component resolvable: the prefab
        /// loader, the cooker's validation hooks, and the editor's asset picker all turn a field's
        /// leaf TypeId back into an asset type through this. A type with no reflected handle leaf
        /// (a Shader, a VertexLayout) leaves it 0 and simply cannot sit on a component.
        ///
        /// Declared as a bare u64 rather than a Veng::TypeId because assetpack carries no
        /// reflection dependency; TypeId is an alias for exactly this type.
        u64 HandleFieldType = 0;
    };

    /// @brief Name ↔ id ↔ display metadata for the asset types a host knows about.
    ///
    /// A plain instance the host owns and threads by reference — never a global: assetpack is a
    /// static library linked into several images, so a global would give each image its own
    /// divergent copy. Dispatch tables key on the AssetTypeId value directly and never consult a
    /// registry; parsing a pack manifest's `"type"` string and rendering a type for a human are
    /// the registry's only jobs.
    ///
    /// Registering two types under one id, one name, or one handle-leaf TypeId is a fatal
    /// collision — the same discipline the reflection TypeRegistry applies.
    class AssetTypeRegistry
    {
    public:
        /// @brief Constructs an empty registry.
        AssetTypeRegistry();

        /// @brief Destroys the registry and the storage it owns.
        ~AssetTypeRegistry();

        /// @brief Move-constructs, taking over the source registry's storage.
        AssetTypeRegistry(AssetTypeRegistry&& other) noexcept;

        /// @brief Move-assigns, taking over the source registry's storage.
        AssetTypeRegistry& operator=(AssetTypeRegistry&& other) noexcept;

        /// @brief Records an asset type, aborting on an id or name collision.
        /// @param info  The type's identity, canonical name, and display metadata.
        void Register(AssetTypeInfo info);

        /// @brief Returns the recorded info for an id, or nullptr when the id is unknown.
        /// @param id  The asset type to look up.
        /// @return The registered info, or nullptr.
        [[nodiscard]] const AssetTypeInfo* Find(AssetTypeId id) const;

        /// @brief Resolves a canonical authoring/manifest type name to its id.
        ///
        /// The one place a manifest's `"type"` string is decoded, shared by the cooker and the
        /// editor's source index.
        /// @param name  The manifest type name (e.g. "MaterialInstance").
        /// @return The matching id, or nullopt when the name is unregistered.
        [[nodiscard]] optional<AssetTypeId> FindByName(std::string_view name) const;

        /// @brief Resolves an AssetHandle\<T\> field's reflected leaf TypeId to the type it references.
        ///
        /// The one mapping the prefab loader, the cooker's cook-time handle validation, and the
        /// editor's asset picker share, so no two of them can answer differently. A game registers
        /// its own pairing by setting AssetTypeInfo::HandleFieldType alongside its loader factory.
        /// @param handleFieldType  The reflected leaf TypeId of an AssetHandle\<T\> field.
        /// @return The asset type the handle references, or nullopt when nothing registered it.
        [[nodiscard]] optional<AssetTypeId> FindByHandleField(u64 handleFieldType) const;

        /// @brief Returns whether an id is registered.
        /// @param id  The asset type to query.
        [[nodiscard]] bool IsRegistered(AssetTypeId id) const;

        /// @brief Canonical manifest name of an id, or its hex spelling when unregistered.
        /// @param id  The asset type to name.
        /// @return The registered name, or "0x…" for an unknown id.
        [[nodiscard]] string GetName(AssetTypeId id) const;

        /// @brief Human-readable name of an id, or its hex spelling when unregistered.
        /// @param id  The asset type to name.
        /// @return The registered display name, or "0x…" for an unknown id.
        [[nodiscard]] string GetDisplayName(AssetTypeId id) const;

        /// @brief Short badge glyph of an id, or "?" when unregistered.
        /// @param id  The asset type to render.
        /// @return The registered glyph, or "?" for an unknown id.
        [[nodiscard]] string GetGlyph(AssetTypeId id) const;

        /// @brief Returns every registered type, keyed by id.
        [[nodiscard]] const std::unordered_map<AssetTypeId, AssetTypeInfo>& All() const;

    private:
        /// @brief The registry's three lookup tables, defined in the implementation TU.
        struct Impl;

        /// @brief The owned storage, held by pointer so an including TU sees no table.
        std::unique_ptr<Impl> m_Impl;
    };

    /// @brief Pre-fills a registry with the eighteen asset types the engine defines.
    ///
    /// Every host calls this on the registry it owns before any other registration, so a
    /// manifest naming a builtin resolves without the consumer re-declaring it.
    /// @param registry  The registry to fill.
    void RegisterBuiltinAssetTypes(AssetTypeRegistry& registry);
}
