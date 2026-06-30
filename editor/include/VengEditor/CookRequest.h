#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Project/BuildConfiguration.h>
#include <Veng/Task/TaskSystem.h>

/// @brief Cook-on-demand contract as seen by libveng_editor.
///
/// The importer table lives in libveng_cook, which libveng_editor never links.
/// The editor exe (which does link libveng_cook) injects a CookBackend; the
/// framework names the request and the function type, keeping importers out of
/// the framework library.

namespace VengEditor
{
    /// @brief A request to cook one source asset on demand.
    ///
    /// SourcePath is the per-asset JSON source (e.g. assets/textures/brick.tex.json).
    /// TargetId is the AssetId the cooked blob is addressable as once mounted.
    struct CookRequest
    {
        /// @brief Absolute path to the per-asset JSON source file.
        Veng::path SourcePath;
        /// @brief AssetId the cooked result will be mounted under.
        Veng::AssetId TargetId;
        /// @brief Asset type, used to select the correct importer.
        Veng::AssetType Type{};

        /// @brief Project source-pack manifests, used to resolve cross-asset references
        /// (e.g. a material's shaders and textures) by AssetId during the cook.
        ///
        /// The project's packs share one AssetId namespace, so the cook resolves an id against
        /// every pack the project owns — an asset edited in one pack may reference an asset in a
        /// sibling. The host also appends the engine core pack manifest, so an asset resolves the
        /// built-in core ids (the standard vertex shaders). Empty for a source with no cross-asset
        /// references. The host fills it; a panel leaves it empty.
        Veng::vector<Veng::path> ReferenceManifests;

        /// @brief Game module reflected for type/system validation during the cook.
        ///
        /// A level cook validates its system ids and config fields against the module's
        /// reflected SystemRegistry/TypeRegistry, so it requires this. Empty for a source
        /// that needs no module reflection (texture, material). The host fills it from its
        /// configured game-module path; a panel leaves it empty.
        Veng::path ModulePath;

        /// @brief Active build configuration resolving a texture's compression role to a format.
        ///
        /// A texture cook resolves its role through this configuration's role table, the same
        /// way the file-based build does. nullopt means no project settings: the importer falls
        /// back to its hardcoded ASTC default, the zero-config behavior. The host fills it from
        /// its ProjectSettings; a panel leaves it unset.
        Veng::optional<Veng::BuildConfiguration> ActiveConfig;

        /// @brief Engine core shader directory added to the cook's Slang search path.
        ///
        /// A material cook reflects a fragment shader that may `#include "Veng/material.slang"`;
        /// this dir lets that cross-pack include resolve, mirroring the file-based build's
        /// `--shader-include`. Empty for a source needing no engine shader header. The host
        /// fills it from the core pack's directory; a panel leaves it empty.
        Veng::path ShaderIncludeDir;
    };

    /// @brief Cook backend the editor exe injects into the host.
    ///
    /// Runs the cook off the render thread on the supplied TaskSystem and returns a
    /// Task carrying the in-memory archive bytes (a single-entry .vengpack) or an
    /// error string. The continuation receives Result<vector<u8>>.
    using CookBackend =
        Veng::function<Veng::Task<Veng::vector<Veng::u8>>(const CookRequest&, Veng::TaskSystem&)>;

    /// @brief AssetId minter the editor exe injects (it links libveng_cook).
    ///
    /// Mints a collision-free id checked against the given reference pack manifests — the
    /// in-process form of `vengc generate-id --reference`, so an editor-authored id is the
    /// same kind of mint the CLI produces. Null leaves authored materials on the hand-mint floor.
    /// @param referencePacks  Pack manifests whose ids the minted id must avoid.
    using AssetIdMinter = Veng::function<Veng::AssetId(std::span<const Veng::path>)>;
}
