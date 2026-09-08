#pragma once

#include <Veng/Veng.h>
#include <Veng/Render/GraphicsSettings.h>
#include <Veng/Renderer/DynamicResolution.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/SceneRendererSettings.h>
#include <Veng/Renderer/Viewport.h>

namespace Veng
{
    class GraphicsSettings;
    struct LevelRenderSettings;

    /// @brief The input to a graphics resolve: the user's chosen values plus the authoring context.
    ///
    /// Handed to Application::OnResolveGraphics so the game can compose the player's quality/cost
    /// preferences with the scene's authored look. It carries the persisted store (the schema and the
    /// chosen option/scalar per setting, read through GraphicsSettings) and the two authoring inputs
    /// the composition needs: the world's authored LevelRenderSettings and the engine's built-in
    /// display selections. The engine does not fix how the two axes combine — that is the game's, in
    /// its resolver; the engine only invokes it and applies the result.
    ///
    /// When no gameplay world is active (boot, a front menu), AuthoredLook is the presented menu
    /// world's LevelRenderSettings if it authors one, else a default-constructed LevelRenderSettings —
    /// so a resolver must be total on the default-constructed input.
    struct GraphicsResolveInput
    {
        /// @brief The per-machine graphics store: the schema and the chosen values (borrowed).
        const GraphicsSettings& Settings;
        /// @brief The engine's built-in display/output selections (a reference into Settings).
        const BuiltinDisplayChoices& Display;
        /// @brief The authored look to compose with; a default-constructed value when no world is active.
        const LevelRenderSettings& AuthoredLook;
    };

    /// @brief The machine-global renderer state a resolve produces, applied once per apply.
    ///
    /// State that is neither viewport-shaped (Viewport::Configure) nor a per-frame view knob: the
    /// engine applies it to the render Context / BindlessRegistry beside the display arms. It
    /// defaults to the values the engine used before the control existed, so an identity resolver
    /// leaves sampling unchanged.
    struct GraphicsGlobalFacet
    {
        /// @brief Whether anisotropic filtering is active on scene-texture samplers.
        bool AnisotropyEnabled = true;
        /// @brief The anisotropy sample count for scene-texture samplers (clamped to the device max).
        f32 MaxAnisotropy = Renderer::DefaultMaxAnisotropy;
        /// @brief The texture-quality mip-skip level applied to the AssetManager's texture build path.
        ///
        /// 0 (the default) uploads the full cooked mip chain; a higher level drops that many top mip
        /// levels from every cappable texture built afterward, uploading a smaller image. Read at
        /// build time, so it is reload-to-apply — resident textures the AssetManager already caches
        /// are not rebuilt, and the change lands on textures loaded fresh (in practice, next start).
        u32 TextureQualityMipSkip = 0;
    };

    /// @brief The output of a graphics resolve: the concrete renderer state the engine applies.
    ///
    /// The engine fills this with the authored baseline (the authored look mapped onto the two
    /// renderer surfaces, and the viewport's current dynamic-resolution choice) before invoking
    /// Application::OnResolveGraphics, so the identity default resolver returns the authored look
    /// unchanged by doing nothing. A game's resolver mutates these fields from the user's chosen
    /// values; the engine then applies Settings through Viewport::Configure (only when a topology
    /// field changed), pushes View's per-frame knobs, applies the dynamic-resolution choice, and
    /// applies the Global facet to the render Context / BindlessRegistry.
    ///
    /// View is Renderer::ViewState — the per-frame knob subset of the renderer's SceneView — and the
    /// engine is the single writer of its display-calibration OutputBrightness/OutputGamma fields
    /// (filled from Display after the resolver runs, so a resolver never sets them).
    struct GraphicsResolveOutput
    {
        /// @brief The topology/sizing surface applied through Viewport::Configure on a change.
        Renderer::SceneRendererSettings Settings;
        /// @brief The per-frame view knobs pushed into the managed viewports.
        Renderer::ViewState View;
        /// @brief The adaptive render-resolution tuning applied when DynamicResolutionEnabled.
        Renderer::DynamicResolutionSettings DynamicResolution;
        /// @brief Whether adaptive render resolution is enabled; false clears it on the viewports.
        bool DynamicResolutionEnabled = false;
        /// @brief The machine-global renderer state applied beside the display arms.
        GraphicsGlobalFacet Global;
    };
}
