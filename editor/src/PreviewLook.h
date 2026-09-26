#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Scene/Components.h>
#include <Veng/Veng.h>

namespace Veng
{
    class EnvironmentMap;
    class Scene;
}

namespace VengEditor
{
    /// @brief How a project wants the editor's previews to look by default: resolved once by the
    /// editor host from project.veng's "preview" block and the project's packs, and shared by the
    /// material preview and the prefab editor's viewport.
    struct PreviewLook
    {
        /// @brief The render block of the level the project previews under — its exposure,
        /// tonemapper, bloom and ambient — or nothing for the renderer's own defaults.
        Veng::optional<Veng::LevelRenderSettings> Render;
        /// @brief The camera's vertical field of view, in radians.
        Veng::f32 FovY = glm::radians(45.0f);
        /// @brief The environment a preview is lit by; the invalid id for none.
        Veng::AssetId Environment;
        /// @brief Every environment the project's packs hold, by display name, for a picker.
        Veng::vector<std::pair<Veng::string, Veng::AssetId>> Environments;
    };

    /// @brief What a preview adds to a scene to light it: an environment sky, a sun, both or
    /// neither.
    struct PreviewLighting
    {
        /// @brief A Sky lighting the scene from the look's environment.
        bool Sky = false;
        /// @brief A directional sun at daylight illuminance.
        bool Sun = false;
    };

    /// @brief How a preview lights `scene`: what the scene already carries is kept, and the
    /// editor fills in the rest.
    ///
    /// An environment becomes the scene's sky when the scene has none of its own. The sun is the
    /// fallback for a scene that would otherwise be unlit: added only when the scene carries no
    /// light and no environment sky is being added, the same rule the material preview opens
    /// with.
    /// @param scene        The scene being previewed.
    /// @param environment  Whether the look's environment is available to light it.
    [[nodiscard]] PreviewLighting PlanPreviewLighting(const Veng::Scene& scene, bool environment);

    /// @brief Adds the lighting PlanPreviewLighting chooses to `scene`, as EditorOnly entities.
    ///
    /// The added sky and sun belong to the editor, not the document: they render but are not
    /// listed or saved.
    /// @param scene        The scene being previewed.
    /// @param environment  The look's environment, loaded; an invalid handle for none.
    void AddPreviewLighting(Veng::Scene& scene,
                            const Veng::AssetHandle<Veng::EnvironmentMap>& environment);
}
