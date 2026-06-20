#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

/// @brief The deferred g-buffer layout — the fixed contract every opaque material
/// pipeline and the geometry pass agree on.
///
/// A material's fragment shader writes these channels (through the GBufferOutput struct
/// in the shader), the geometry pass clears and stores them, and the lighting pass
/// samples them.
///
/// The layout is albedo (G0) / world-normal (G1) / packed ORM (G2) plus a sampled
/// depth attachment the lighting pass reconstructs world position from. This is the
/// opaque material contract — a transparent/forward material outputs final color
/// through a separate fragment entry, not a change to this one.
///
/// Color-space contract: albedo is stored sRGB-encoded (RGBA8Srgb) and sampled as
/// linear — the sampler decodes on read, so a material writes its sampled albedo
/// straight to G0 and the lighting pass reads it back in linear space for BRDF math.
/// Normals are world-space, stored in a signed float format so the full [-1, 1] range
/// survives without an encode/decode round-trip. The ORM channels are perceptually fine
/// at 8-bit unorm.
namespace Veng::Renderer
{
    /// @brief The g-buffer's format and usage constants.
    ///
    /// The geometry pass renders into G0/G1/G2 (+ depth); a fullscreen pass samples them.
    namespace GBuffer
    {
        /// @brief G0 — base color. rgb is the sRGB-encoded albedo; a is unused (reserved).
        ///
        /// Only G2.a carries data (emissive strength), so this is the one free g-buffer
        /// channel before independent colored emissive needs a fourth target.
        inline constexpr Format AlbedoFormat = Format::RGBA8Srgb;

        /// @brief G1 — world-space normal in xyz.
        inline constexpr Format NormalFormat = Format::RGBA16Sfloat;

        /// @brief G2 — packed material params: R=occlusion, G=roughness, B=metallic, A=emissive strength.
        ///
        /// RGBA8: roughness/metallic/occlusion are perceptually fine at 8-bit; emissive strength
        /// is a low-dynamic scalar (the lighting pass scales albedo by it).
        inline constexpr Format ORMFormat = Format::RGBA8Unorm;

        /// @brief The depth attachment, also sampled by the lighting pass for world-position reconstruction.
        ///
        /// The lighting pass reconstructs world position via the camera's inverse view-projection.
        /// This and the shadow atlases are the depth targets read as textures in the engine.
        inline constexpr Format DepthFormat = Format::D32Sfloat;

        /// @brief Usage flags for the three color attachments (G0/G1/G2).
        ///
        /// Written by the geometry pass and sampled by downstream fullscreen passes.
        /// RenderGraph::Compile asserts a sampled target carries ImageUsage::Sampled,
        /// so these flags are load-bearing.
        inline constexpr ImageUsage ColorUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;

        /// @brief Usage flags for the depth attachment.
        ///
        /// Acts as both a depth attachment for the geometry pass and a sampled source for
        /// the lighting pass. These flags are load-bearing (see ColorUsage).
        inline constexpr ImageUsage DepthUsage = ImageUsage::DepthAttachment | ImageUsage::Sampled;
    }
}
