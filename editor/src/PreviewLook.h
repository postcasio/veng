#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Scene/Components.h>
#include <Veng/Veng.h>

namespace Veng
{
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
        Veng::optional<Veng::RenderLook> Render;
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

    /// @brief Lights `scene` as PlanPreviewLighting does with no environment: a sun, as an
    /// EditorOnly entity, when the scene carries no light.
    ///
    /// The lighting of a scene with no preview look (a level's world). The sun belongs to the
    /// editor, not the document: it renders but is not listed or saved.
    /// @param scene  The scene being previewed.
    void AddPreviewLighting(Veng::Scene& scene);

    /// @brief What a scene viewport's lighting controls edit: the editor's own sky and sun, and
    /// the exposure over the look's.
    struct SceneLighting
    {
        /// @brief The environment the editor's sky shows; the invalid id for no editor sky.
        Veng::AssetId Environment;
        /// @brief Scales the editor sky's background and ambient radiance.
        Veng::f32 EnvIntensity = 1.0f;
        /// @brief Stops over the look's exposure.
        Veng::f32 Exposure = 0.0f;
        /// @brief Whether the editor's sun lights the scene.
        bool Sun = false;
        /// @brief The sun's azimuth about +Y, radians.
        Veng::f32 SunYaw = 0.8f;
        /// @brief The sun's elevation above the horizon, radians.
        Veng::f32 SunPitch = 0.7f;
        /// @brief The sun's illuminance, in units of 10,000 lux.
        Veng::f32 SunIntensity = 3.0f;
        /// @brief The sun's colour.
        Veng::vec3 SunColor{1.0f, 0.96f, 0.9f};

        friend bool operator==(const SceneLighting&, const SceneLighting&) = default;
    };

    /// @brief The lighting a scene viewport opens `scene` with, following PlanPreviewLighting.
    ///
    /// The environment lights the scene unless it carries a sky of its own; the sun is on, at
    /// daylight, only when the scene would otherwise be unlit.
    /// @param scene        The scene being previewed, before the editor adds any lighting to it.
    /// @param environment  The look's environment when it loaded; the invalid id otherwise.
    [[nodiscard]] SceneLighting DefaultSceneLighting(const Veng::Scene& scene,
                                                     Veng::AssetId environment);
}
