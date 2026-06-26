#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class Scene;
    struct LevelRenderSettings;
}

namespace Veng::Renderer
{
    class Viewport;
    struct ViewState;
    struct SceneRendererSettings;
}

namespace Veng
{
    /// @brief Resolves a scene's camera at a viewport's aspect and pushes its per-frame render source.
    ///
    /// The gameplay→render bridge that keeps Renderer::Viewport gameplay-agnostic: it reads the
    /// viewport's current output extent for the aspect, resolves the scene's primary camera through
    /// ResolvePrimaryCameraView (falling back to DefaultCameraView when the scene resolves none),
    /// fills a ViewState (the caller's tone/bloom/environment knobs plus the scene, camera, and
    /// delta), and pushes it via Viewport::SetViewState. A managed game world calls this every
    /// frame; a game owning its own viewports or a second seat calls it directly.
    /// @param viewport  The viewport to push into; its output extent supplies the aspect.
    /// @param scene     The scene to render and resolve the camera from.
    /// @param knobs     The per-frame tone/bloom/environment values to carry; World/Camera/Delta
    ///                  are overwritten by this call.
    /// @param delta     Frame delta in seconds, forwarded to the renderer.
    void PushSceneView(Renderer::Viewport& viewport, const Scene& scene,
                       const Renderer::ViewState& knobs, f32 delta = 0.0f);

    /// @brief Maps a level's render settings onto a renderer's topology knobs and per-frame view.
    ///
    /// Splits LevelRenderSettings across the two renderer surfaces it feeds: the topology toggles
    /// (Bloom / Shadows / AO / Skybox) onto a SceneRendererSettings the caller applies through
    /// Viewport::Configure, and the per-frame values (Exposure / BloomIntensity / Environment /
    /// EnvironmentIntensity) onto a ViewState the caller pushes each frame. The single mapping both
    /// example games and the managed world share, so the level→renderer wiring lives in one place.
    /// @param render    The level's render-settings subset.
    /// @param settings  The topology/sizing knobs to update (the toggles are written in place).
    /// @param view      The per-frame view to seed (the tone/bloom/environment values are written).
    void ApplyLevelRenderSettings(const LevelRenderSettings& render,
                                  Renderer::SceneRendererSettings& settings,
                                  Renderer::ViewState& view);
}
