#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    class Scene;
    struct RenderLook;
}

namespace Veng::Renderer
{
    class Viewport;
    struct ViewState;
    struct SceneRendererSettings;
}

namespace Veng
{
    /// @brief The note an authoring surface shows beside inert depth-of-field lens fields.
    ///
    /// Shown whenever ViewState::DofFromPhysicalCamera is set: the focus distance and aperture are
    /// authored by the camera, so a stored value is recorded but not consulted. Shared so the
    /// render-settings panel and the level editor say the same thing.
    inline constexpr string_view DofPhysicalCameraNote =
        "Focus and aperture come from the active Physical camera's lens; these values are stored "
        "but not used until it stops being Physical.";

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
    /// @param knobs     The per-frame tone/bloom/environment values to carry; World/Camera/Delta/Alpha
    ///                  are overwritten by this call.
    /// @param delta     Frame delta in seconds, forwarded to the renderer.
    /// @param alpha     The fixed-timestep interpolation fraction in [0, 1), forwarded to the gather.
    void PushSceneView(Renderer::Viewport& viewport, const Scene& scene,
                       const Renderer::ViewState& knobs, f32 delta = 0.0f, f32 alpha = 0.0f);

    /// @brief Fills a pushed view state's depth-of-field fields from its resolved camera and target.
    ///
    /// The one site the defocus parameters are resolved, applied to the per-frame copy a viewport
    /// glue pushes rather than to any stored knob: a Physical camera's lens supplies the focus
    /// distance and aperture (so the camera wins by construction on every frame, while the
    /// authored values survive underneath and come back if the camera stops being Physical), and
    /// any camera supplies the sensor-to-pixel CoC scale, which is never hand-authored. It is also
    /// the authoritative gate on the two quality knobs: DofMaxCoc and DofRingCount are hard-clamped
    /// here, because a cooked level is untrusted input and an unclamped ring count is an unbounded
    /// GPU loop.
    /// @param state               The view state being pushed; its DoF fields are overwritten.
    /// @param viewportPixelHeight The target's vertical extent in pixels.
    void ResolveDofViewState(Renderer::ViewState& state, f32 viewportPixelHeight);

    /// @brief Maps a render look onto a renderer's topology and per-frame view.
    ///
    /// Splits RenderLook across the two renderer surfaces it feeds: the topology toggles (Bloom /
    /// Shadows / AO / …) onto a SceneRendererSettings applied through Viewport::Configure, and the
    /// per-frame values (Exposure / BloomIntensity / …) onto a ViewState. The sky/environment knobs
    /// are not here — they are the Sky and TimeOfDay scene components, resolved by the renderer
    /// itself. The single mapping a viewport's default look resolve, a host's graphics resolve and
    /// the editor's authoring previews share, so the look→renderer wiring lives in one place. The
    /// fields it writes onto @p view are exactly the ones CopyLookKnobs carries.
    /// @param look      The look to map.
    /// @param settings  The topology/sizing knobs to update (the look's toggles are written in place).
    /// @param view      The per-frame view to update (the look's per-frame values are written).
    void ApplyRenderLook(const RenderLook& look, Renderer::SceneRendererSettings& settings,
                         Renderer::ViewState& view);

    /// @brief Writes the look-owned per-frame fields of a resolved view over another view.
    ///
    /// The per-frame half of a viewport's look: the fields ApplyRenderLook writes onto a view,
    /// copied from @p resolved (the view a look resolve produced) onto @p view (the one pushed this
    /// frame), leaving every other field — the scene, camera, delta and alpha, and the knobs a look
    /// does not own — as pushed. The lens fields (DofFocusDistance, DofAperture) are left alone
    /// while @p view's camera authored them (ViewState::DofFromPhysicalCamera), and the two
    /// depth-of-field quality knobs are clamped again, since a resolver may have written them.
    /// @param resolved  The view carrying the resolved look's per-frame values.
    /// @param view      The view to write them over.
    void CopyLookKnobs(const Renderer::ViewState& resolved, Renderer::ViewState& view);
}
