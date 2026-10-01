#pragma once

#include <span>

#include <Veng/Renderer/GatherPass.h>
#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief What the swap-chain composite samples as its scene for one frame.
    enum class CompositeSceneSource : u8
    {
        /// @brief The gather's assembly target: the placements need assembling.
        Gather,
        /// @brief The last placement's texture, sampled directly: it covers the whole window.
        Direct,
        /// @brief A black stand-in: there are no placements to show.
        Black,
    };

    /// @brief What the swap-chain composite blends as its overlay for one frame.
    enum class CompositeOverlaySource : u8
    {
        /// @brief The overlay layer's own output image.
        Layer,
        /// @brief A transparent stand-in, so the composite returns the scene unchanged.
        Transparent,
    };

    /// @brief Decides the composite's scene source from the frame's placements.
    ///
    /// The gather draws placements in list order with an opaque blend, so a last placement whose
    /// region is the whole window overwrites everything before it, and the gather's linear,
    /// clamp-to-edge, full-UV lookup into it is the same lookup the composite makes when sampling
    /// it directly. Assembling is then a full-window copy that changes nothing, and is skipped.
    /// With no placements the gather would write only its clear colour.
    /// @param placements       The frame's Presented placements, in gather (list) order.
    /// @param swapChainExtent  The window's framebuffer extent the composite writes.
    /// @return Direct when the last placement covers the window exactly, Black when there are no
    ///         placements, Gather otherwise.
    [[nodiscard]] CompositeSceneSource
    ResolveCompositeSceneSource(std::span<const CompositePlacement> placements,
                                uvec2 swapChainExtent);

    /// @brief Decides the composite's overlay source from whether the overlay layer drew.
    ///
    /// A layer that drew nothing leaves its output image unwritten, so the composite blends a
    /// transparent stand-in in its place rather than an image holding a stale or undefined frame.
    /// @param layerDrew  Whether the overlay layer recorded any drawing this frame.
    /// @return Layer when it drew, Transparent otherwise.
    [[nodiscard]] CompositeOverlaySource ResolveCompositeOverlaySource(bool layerDrew);
}
