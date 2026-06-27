#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

/// @brief Constants for the SceneRenderer entity-id picking pass.
///
/// Kept out of the GBuffer.h opaque material contract: picking is an optional,
/// off-by-default authoring concern, and its id target is bound only by the picking
/// pipeline variant's own RenderingInfo — never by the shipping geometry pass. The
/// shipping g-buffer (G0/G1/G2/G3 + depth) is byte-for-byte unchanged whether or not
/// picking is enabled.
namespace Veng::Renderer::Picking
{
    /// @brief Color format of the entity-id target.
    ///
    /// A single 32-bit unsigned integer: the pick id (packed entity index + 1) the
    /// picking fragment writes per covered texel. 0 is the cleared no-hit sentinel.
    inline constexpr Format EntityIdFormat = Format::R32Uint;

    /// @brief Usage flags for the entity-id target.
    ///
    /// A color attachment the picking pass writes and a transfer source the async
    /// readback copies the requested texel neighborhood from.
    inline constexpr ImageUsage EntityIdUsage =
        ImageUsage::ColorAttachment | ImageUsage::TransferSrc;

    /// @brief The cleared no-hit value: read back as "background / no entity".
    ///
    /// The pick id is the packed entity index + 1, so a live entity is always >= 1 and 0
    /// can never collide with one. The readback subtracts 1 and resolves the index back to
    /// the live Entity at resolve time.
    inline constexpr u32 NoEntityId = 0;

    /// @brief Half-extent in texels of the screen-space search window the readback samples.
    ///
    /// The exact texel under the cursor wins when non-zero; otherwise the readback expands
    /// to the (2*SearchRadius + 1)² window and takes the front-most non-zero id, ties broken
    /// by nearest to the cursor. A small fixed "click-near" forgiveness for thin silhouettes.
    inline constexpr i32 SearchRadius = 3;
}
