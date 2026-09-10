#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>

namespace Veng
{
    /// @brief The engine's closed render-visibility layer table.
    ///
    /// Every drawable sits on exactly one layer (its MeshRenderer::Layer), and a view draws a layer
    /// only when the view's own layer mask names it. This is the render-side counterpart of the
    /// physics collision layers (PhysicsLayer) and shares their doctrine: the set is deliberately
    /// **closed** — a filter is what silently drops geometry when it drifts, and a table that fits on
    /// one screen is worth more than an extensible one. A consumer selects which layers a view draws;
    /// it never adds a layer. The engine extends the table only with a layer that has a general,
    /// game-agnostic justification of its own.
    ///
    /// Integer values are stable — persisted in prefabs.
    enum class RenderLayer : u32
    {
        /// @brief Ordinary scene geometry — the surroundings a body, a light, or a probe sees.
        Default = 0,
        /// @brief Decoration slaved to the viewing camera: a near-field particle shell, a billboard,
        ///        any mesh re-placed each frame relative to the eye rather than fixed in the scene.
        ///
        /// A camera-anchored drawable has no fixed place in the world, so it is not part of the
        /// environment an environment probe captures from a point in the scene — a reflection or a
        /// lens sampling such a capture would show it floating behind everything, at a distance it
        /// was never at. Environment captures therefore drop it by default (see
        /// DefaultEnvironmentCaptureLayers), while the ordinary camera view draws it like anything
        /// else.
        ViewAnchored = 1,
        /// @brief Distant background geometry — the far surroundings a reflection probe reflects as
        ///        the scene's environment: a sky's celestial bodies, a distant skyline, any backdrop
        ///        a positioned probe should see as its surroundings rather than as nearby content.
        ///
        /// The distinction from Default is exactly what a reflection probe placed *within* the scene
        /// reflects. Such a probe sits amid the local (Default) geometry — often on the very surface
        /// it is lighting — so that geometry is not its environment; the distant backdrop is. An
        /// image-based-lighting probe that wants only its surroundings names this layer alone
        /// (RenderLayerBit(Environment)), so the local geometry it sits within never convolves into
        /// the light it casts back on that geometry. A general specular reflection keeps Default too
        /// (see DefaultEnvironmentCaptureLayers), mirroring nearby geometry as well. The ordinary
        /// camera view draws it like Default.
        Environment = 2,
    };

    /// @brief Number of members in the closed RenderLayer table.
    inline constexpr u32 RenderLayerCount = 3;

    /// @brief The bit a layer occupies in a render-layer mask.
    /// @param layer  The layer whose bit to compute.
    /// @return A single-bit mask.
    [[nodiscard]] constexpr u32 RenderLayerBit(const RenderLayer layer)
    {
        return 1U << static_cast<u32>(layer);
    }

    /// @brief A mask naming every render layer — the default a view draws, changing nothing.
    inline constexpr u32 AllRenderLayers = (1U << RenderLayerCount) - 1U;

    /// @brief Whether @p mask names @p layer, i.e. a view carrying it draws that layer.
    /// @param mask   The view's render-layer mask.
    /// @param layer  The drawable's layer.
    /// @return True when the layer's bit is set in the mask.
    [[nodiscard]] constexpr bool RenderLayerInMask(const u32 mask, const RenderLayer layer)
    {
        return (mask & RenderLayerBit(layer)) != 0;
    }

    /// @brief The layers a reflection capture draws unless a consumer names another set.
    ///
    /// Every layer but ViewAnchored: a probe captures the scene around a point, and camera-anchored
    /// decoration is not part of that scene (see RenderLayer::ViewAnchored). This carries only the
    /// *universal* exclusion — ViewAnchored is wrong in every capture. It keeps both Default (nearby
    /// scene geometry) and Environment (the distant backdrop), which is what a general specular
    /// reflection wants; a capture that wants only the distant surroundings — an IBL probe sitting
    /// within the local geometry — names RenderLayer::Environment alone instead.
    inline constexpr u32 DefaultEnvironmentCaptureLayers =
        AllRenderLayers & ~RenderLayerBit(RenderLayer::ViewAnchored);
}

VE_ENUM(::Veng::RenderLayer, 0x357D35AA53685C1AULL)
VE_ENUMERATOR(Default)
VE_ENUMERATOR(ViewAnchored)
VE_ENUMERATOR(Environment)
VE_ENUM_END();
