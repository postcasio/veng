#pragma once

#include <Veng/Asset/Archive.h>
#include <Veng/Cook/AssetPack.h>
#include <Veng/Cook/Importer.h>

#include <set>
#include <span>
#include <unordered_map>

/// @brief Pack JSON schema understood by the cooker.
///
/// `{ "version": 1, "assets": [ { "id": <u64>, "type": "<name>", ... }, ... ] }`
/// - `"id"` — author-owned AssetId (any non-zero u64); 0 and duplicates are cook errors.
/// - `"type"` — selects the registered AssetImporter (`"raw"`, `"texture"`, `"mesh"`,
///   `"shader"`, `"material"`, `"vertex_layout"`); unknown or unregistered types are cook errors.
/// - Per-type fields (e.g. `"source"`) are interpreted by the importer.

namespace Veng::Cook
{
    /// @brief Parses a pack JSON file into an AssetPack (id/type/source registry).
    ///
    /// Used for cross-pack resolution and AssetId generation.
    /// Errors are located: `"pack '<path>': <reason>"`.
    /// @param packJson  Path to the pack JSON file.
    [[nodiscard]] Result<AssetPack> ParseAssetPack(const path& packJson);

    /// @brief Writes a GCC-style depfile declaring `target` as depending on every path in `dependencies`.
    ///
    /// Intended for `add_custom_command(DEPFILE ...)` so the cook re-runs when any recorded
    /// source changes. Paths with spaces are backslash-escaped.
    /// Errors are located: `"depfile '<path>': <reason>"`.
    /// @param depfilePath   Output depfile path.
    /// @param target        The build target (left-hand side of the depfile rule).
    /// @param dependencies  Every source path the cook read.
    [[nodiscard]] VoidResult WriteDepfile(const path& depfilePath, const path& target,
                                          std::span<const path> dependencies);

    /// @brief Owns the importer table and turns a pack JSON into a .vengpack archive.
    class Cooker
    {
    public:
        /// @brief Registers an importer for its declared AssetType, replacing any prior registration.
        /// @param importer  The importer to register.
        void Register(Unique<AssetImporter> importer);

        /// @brief Parses packJson, cooks every entry through its registered importer, and writes the archive.
        ///
        /// `referencePacks` lists additional uncooked pack JSON files whose assets resolve by AssetId
        /// during cooking (e.g. a core layout pack). `types` carries the loaded module's reflected
        /// component descriptors — non-null only on a `--module` cook — passed to importers via
        /// CookContext. When `outDependencies` is non-null it receives every source file read
        /// (manifest, reference packs, per-asset JSONs, binary payloads), sorted and de-duplicated.
        /// Errors are located: `"pack '<path>': asset[<n>]: <reason>"`.
        /// @param packJson        Path to the pack JSON to cook.
        /// @param outArchive      Destination .vengpack path.
        /// @param referencePacks  Additional packs available for cross-asset AssetId resolution.
        /// @param types           Reflected module type registry, or nullptr for non-module cooks.
        /// @param systems         Reflected module system catalog (level id validation), or nullptr.
        /// @param outDependencies If non-null, receives the sorted, de-duplicated dependency list.
        [[nodiscard]] VoidResult CookPack(const path& packJson, const path& outArchive,
                                          std::span<const path> referencePacks = {},
                                          const TypeRegistry* types = nullptr,
                                          const SystemRegistry* systems = nullptr,
                                          vector<path>* outDependencies = nullptr) const;

        /// @brief Cooks one source asset and returns a complete single-entry .vengpack as in-memory bytes.
        ///
        /// The importer reads `sourcePath` and any referenced files relative to its directory.
        /// `referencePacks` enables cross-asset AssetId resolution (e.g. a material resolving its
        /// shaders and textures). No files are written. This is the cook-on-demand path used by the editor.
        /// Errors are located: `"cook '<source>': <reason>"`.
        /// @param sourcePath     Per-asset JSON source file.
        /// @param id             AssetId to assign the cooked entry.
        /// @param type           Asset type, selects the importer.
        /// @param referencePacks Additional packs for cross-asset AssetId resolution.
        /// @param types          Reflected module type registry, or nullptr.
        /// @param systems        Reflected module system catalog (level id validation), or nullptr.
        [[nodiscard]] Result<vector<u8>> CookSource(const path& sourcePath, AssetId id,
                                                    AssetType type,
                                                    std::span<const path> referencePacks = {},
                                                    const TypeRegistry* types = nullptr,
                                                    const SystemRegistry* systems = nullptr) const;

    private:
        /// @brief Cooks one pack entry JSON into the archive, enforcing id uniqueness.
        [[nodiscard]] VoidResult CookEntry(const CookContext& context, const json& entry,
                                           std::set<u64>& seenIds, ArchiveWriter& writer) const;

        /// @brief Registered importers keyed by AssetType.
        std::unordered_map<AssetType, Unique<AssetImporter>> m_Importers;
    };
}
