#pragma once

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Renderer/SceneRendererSettings.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Veng.h>

#include "PreviewLook.h"

namespace Veng
{
    class AssetManager;
    class EnvironmentMap;
    class ImGuiLayer;
    class ImGuiTexture;
    class MaterialInstance;
    class Mesh;
    class Scene;
    class TypeRegistry;

    namespace Renderer
    {
        class Context;
        class ImageView;
        class Sampler;
    }
}

namespace VengEditor
{
    /// @brief The shapes a material preview can wear.
    enum class MaterialPreviewShape : Veng::u8
    {
        Sphere,
        Cube,
        Plane,
        Cylinder,
        Torus,
        Count,
    };

    /// @brief What a material preview's controls edit: the shape, the channel shown, the
    /// lighting, the look's adjustments and the orbit camera.
    struct MaterialPreviewState
    {
        MaterialPreviewShape Shape = MaterialPreviewShape::Sphere;
        /// @brief The renderer's view: the lit result, or one g-buffer channel.
        Veng::Renderer::DebugView View = Veng::Renderer::DebugView::Final;
        /// @brief UV repeats across the shape.
        Veng::f32 Tiling = 1.0f;

        Veng::AssetId Environment;
        Veng::f32 EnvIntensity = 1.0f;
        /// @brief Rotation of the environment about +Y, radians.
        Veng::f32 EnvRotation = 0.0f;
        /// @brief Stops over the look's exposure.
        Veng::f32 Exposure = 0.0f;

        bool Sun = false;
        Veng::f32 SunYaw = 0.8f;
        Veng::f32 SunPitch = 0.7f;
        /// @brief In units of 10,000 lux.
        Veng::f32 SunIntensity = 3.0f;
        Veng::vec3 SunColor{1.0f, 0.96f, 0.9f};

        bool Bloom = true;
        Veng::f32 BloomStrength = 1.0f;
        Veng::f32 BloomRadius = 1.0f;

        Veng::f32 FovY = glm::radians(45.0f);
        Veng::f32 Yaw = 0.6f;
        Veng::f32 Pitch = 0.3f;
        Veng::f32 Distance = 3.2f;

        friend bool operator==(const MaterialPreviewState&, const MaterialPreviewState&) = default;
    };

    /// @brief Renders one material on a chosen shape through an Offscreen viewport, with an orbit
    /// camera and the controls to light and inspect it.
    ///
    /// Owns a one-shape Scene (the shape, a directional sun, and a Sky wearing the chosen
    /// environment for image-based lighting), an Offscreen Veng::Renderer::Viewport, and the
    /// ImGuiTexture the panel draws. SetMaterial swaps the previewed material after a recook hands
    /// back a fresh handle. The look — exposure, tonemapper, bloom, field of view, environment —
    /// starts from the project's (PreviewLook) and the controls adjust it.
    ///
    /// The environment cannot rotate, so rotating it turns the shape, camera and sun the other
    /// way. The viewport is rendered by the engine drive-list (its owning panel registers it on
    /// this preview's behalf); each frame the preview pushes its view through Update, and Draw
    /// shows the result with its controls and turns drags on it into camera moves.
    class MaterialPreview
    {
    public:
        /// @param context  Renderer context owning all GPU resources.
        /// @param assets   Asset manager used to adopt the shape and load environments.
        /// @param imgui    ImGui layer used to register the output texture.
        /// @param extent   Render resolution in pixels.
        /// @param look     The project's defaults; must outlive the preview.
        MaterialPreview(Veng::Renderer::Context& context, Veng::AssetManager& assets,
                        Veng::ImGuiLayer& imgui, Veng::uvec2 extent, const PreviewLook& look);
        ~MaterialPreview();

        MaterialPreview(const MaterialPreview&) = delete;
        MaterialPreview& operator=(const MaterialPreview&) = delete;

        /// @brief Returns the owned viewport so the owning panel can register it.
        [[nodiscard]] Veng::Renderer::Viewport& GetViewport() const { return *m_Viewport; }

        /// @brief Swaps the previewed material.
        /// @param material  Fresh handle returned by a recook.
        void SetMaterial(Veng::AssetHandle<Veng::MaterialInstance> material);

        /// @brief Pushes this frame's view onto the viewport (and applies control changes).
        ///
        /// The engine drive-list renders the registered viewport; this records no scene render.
        void Update();

        /// @brief Draws the controls and the preview `width` points wide (square), turning drags
        /// on it into camera moves: drag orbits, wheel zooms, right-drag turns the environment
        /// (with Shift, the sun), double-click resets the view.
        void Draw(Veng::f32 width);

        /// @brief Returns the ImGuiTexture for use with UI::Image.
        [[nodiscard]] const Veng::Ref<Veng::ImGuiTexture>& GetTexture() const;

        /// @brief Returns the current render extent.
        [[nodiscard]] Veng::uvec2 GetExtent() const { return m_Extent; }

        /// @brief The controls' current state.
        [[nodiscard]] MaterialPreviewState& State() { return m_State; }

        /// @brief The state a preview opens with under `look`.
        [[nodiscard]] static MaterialPreviewState DefaultState(const PreviewLook& look);

    private:
        void BuildScene();
        void RebuildShape();
        void DrawSettings();

        Veng::Renderer::Context& m_Context;
        Veng::AssetManager& m_Assets;
        Veng::ImGuiLayer& m_ImGui;
        const PreviewLook& m_Look;

        /// @brief Private TypeRegistry for the preview scene's builtin components.
        Veng::Unique<Veng::TypeRegistry> m_Types;
        Veng::Unique<Veng::Scene> m_Scene;
        Veng::Unique<Veng::Renderer::Viewport> m_Viewport;
        Veng::CameraView m_Camera;

        /// @brief The look's topology and per-frame view, which the controls adjust.
        Veng::Renderer::SceneRendererSettings m_BaseSettings;
        Veng::Renderer::ViewState m_BaseView;
        /// @brief The topology last configured (bloom, debug view), so it changes only on edits.
        Veng::Renderer::SceneRendererSettings m_Configured;

        Veng::Ref<Veng::Mesh> m_Shape;
        Veng::Entity m_ShapeEntity;
        Veng::Entity m_SunEntity;
        Veng::Entity m_SkyEntity;
        Veng::AssetHandle<Veng::MaterialInstance> m_Material;
        Veng::AssetHandle<Veng::EnvironmentMap> m_Environment;
        Veng::AssetId m_LoadedEnvironment;

        Veng::Ref<Veng::Renderer::Sampler> m_SceneSampler;
        Veng::Ref<Veng::ImGuiTexture> m_SceneTexture;
        /// @brief The viewport output m_SceneTexture shows.
        Veng::Ref<Veng::Renderer::ImageView> m_ShownOutput;
        Veng::uvec2 m_Extent{};

        MaterialPreviewState m_State;
        /// @brief The shape and tiling the mesh was built for.
        MaterialPreviewShape m_BuiltShape = MaterialPreviewShape::Count;
        Veng::f32 m_BuiltTiling = 0.0f;
        /// @brief A drag that began on the preview, carried until its buttons are released.
        bool m_Dragging = false;
    };
}
