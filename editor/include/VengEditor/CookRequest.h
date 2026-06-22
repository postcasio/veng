#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/AssetType.h>
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

        /// @brief Project source-pack manifest, used to resolve cross-asset references
        /// (e.g. a material's shaders and textures) by AssetId during the cook.
        ///
        /// Empty for a source with no cross-asset references. The host fills it from
        /// its configured manifest path; a panel leaves it empty.
        Veng::path ReferenceManifest;

        /// @brief Game module reflected for type/system validation during the cook.
        ///
        /// A level cook validates its system ids and config fields against the module's
        /// reflected SystemRegistry/TypeRegistry, so it requires this. Empty for a source
        /// that needs no module reflection (texture, material). The host fills it from its
        /// configured game-module path; a panel leaves it empty.
        Veng::path ModulePath;
    };

    /// @brief Cook backend the editor exe injects into the host.
    ///
    /// Runs the cook off the render thread on the supplied TaskSystem and returns a
    /// Task carrying the in-memory archive bytes (a single-entry .vengpack) or an
    /// error string. The continuation receives Result<vector<u8>>.
    using CookBackend =
        Veng::function<Veng::Task<Veng::vector<Veng::u8>>(const CookRequest&, Veng::TaskSystem&)>;
}
