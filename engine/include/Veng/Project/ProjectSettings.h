#pragma once

#include <Veng/Veng.h>
#include <Veng/Path.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Project/BuildConfiguration.h>

namespace Veng
{
    /// @brief How the editor previews an asset (a material on a shape, a prefab in its editor) by
    /// default, so a preview looks as the project's scenes do rather than as the renderer's
    /// defaults.
    struct ProjectPreviewSettings
    {
        /// @brief A Level whose render block — exposure, tonemapper, bloom, ambient — previews
        ///        render under; the invalid id leaves the renderer's defaults.
        AssetId Level;
        /// @brief The preview camera's vertical field of view, in radians; 0 leaves the editor's.
        f32 FovY = 0.0f;
        /// @brief The Environment a preview opens lit by; the invalid id opens with none.
        AssetId Environment;
    };

    /// @brief The project-wide build settings: the set of build configurations and the active one.
    ///
    /// One per project, lived in the JSON authoring file `project.veng`. Holds
    /// project-wide invariants only — no codec here; the codec lives on each
    /// BuildConfiguration. Reflected so the editor lists/edits the configurations
    /// through the property table; Configurations is a genuine reflected array, so
    /// adding or removing a configuration is reflection, not a fixed-capacity hack.
    struct ProjectSettings
    {
        /// @brief The project's build configurations, one per ship target.
        vector<BuildConfiguration> Configurations;
        /// @brief The Name of the configuration the editor previews through and the cook defaults to.
        string ActiveConfiguration;
        /// @brief The asset-pack manifests the project owns, relative to project.veng's directory.
        ///
        /// The cook reads them to cook each pack and to write their mount names into the cooked
        /// project file; the editor reads them to build its AssetId→source index. Persisted by hand
        /// through project.veng's "packs" key, not reflection — kept off the reflected field list so
        /// the editor's build-policy property table stays focused on the configurations.
        vector<path> Packs;
        /// @brief Asset-pack manifests only the editor mounts, relative to project.veng's directory.
        ///
        /// Authoring aids a shipped game has no use for — preview environments, reference assets.
        /// The cook cooks them with the project (they share its AssetId namespace) but leaves them
        /// out of the cooked project file, so the runtime never mounts them; the build copies them
        /// into an editor/ directory beside the launcher, apart from what ships. Persisted by hand
        /// through project.veng's "editorPacks" key, like Packs.
        vector<path> EditorPacks;
        /// @brief How the editor's previews look by default. Persisted by hand through
        /// project.veng's "preview" object ("level", "fovY", "environment"), like Packs.
        ProjectPreviewSettings Preview;
        /// @brief The Level the engine bootstraps when a managed game world mounts the project.
        ///
        /// The cook writes it into the cooked project file (.vengproj); the runtime reads it back on
        /// load and loads it as the startup level. The invalid id (the default) means the project
        /// declares no startup level. Persisted by hand through project.veng's "startupLevel" key
        /// (a decimal AssetId), not reflection — kept off the reflected field list so the editor's
        /// build-policy property table stays focused on the configurations.
        AssetId StartupLevel;

        /// @brief The InputMap the project's Gui documents navigate from.
        ///
        /// Its role-tagged actions (ActionRole) are the navigation every interactive document answers
        /// to, beneath each seat's own contexts (Application::SetDefaultUiContext). The cook writes it
        /// into the cooked project file and a game takes it from there; the editor host reads it from
        /// here, so documents navigate the same under the editor's Play as in the shipped game. The
        /// invalid id (the default) binds nothing: documents navigate by pointer alone. Persisted by
        /// hand through project.veng's "defaultUiContext" key (a hex AssetId string), like
        /// StartupLevel.
        AssetId DefaultUiContext;

        /// @brief The logical name of the game module the editor loads (e.g. "template").
        ///
        /// Resolved to a shared library beside the project's build output — lib<Module>.so /
        /// lib<Module>.dylib / <Module>.dll — and dlopen'd by the editor to register the game's
        /// types, systems, and Application factory. Persisted by hand through project.veng's
        /// "module" key, not reflection (kept off the build-policy property table). Empty when the
        /// project names no module; the runtime launcher bakes its own module name and ignores this.
        string Module;

        /// @brief The logical name of the optional editor-extension module, or empty for none.
        ///
        /// Resolved like Module and dlopen'd by the editor host with a non-null EditorRegistry, so
        /// the game registers editor-only panels and field widgets. Persisted by hand through
        /// project.veng's "editorModule" key. Empty when the project ships no editor extension.
        string EditorModule;
    };
}

VE_REFLECT(::Veng::ProjectSettings, 0xF5D8A1D2C10BC541ULL)
VE_ARRAY_FIELD(Configurations, .DisplayName = "Configurations")
VE_FIELD(ActiveConfiguration, .DisplayName = "Active Configuration")
VE_REFLECT_END();
