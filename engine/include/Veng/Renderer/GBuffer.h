#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

// The deferred g-buffer layout — the fixed contract every opaque material
// pipeline and the geometry pass agree on. A material's fragment shader writes
// these channels (through the GBufferOutput struct in the shader), the geometry
// pass clears and stores them, and the lighting pass samples them.
//
// This is the v1 minimum channel set, designed to grow: PBR shading later adds a
// roughness/metallic/AO target (a future G2) that extends the shader's
// GBufferOutput in one place. It is the OPAQUE material contract — a future
// transparent/forward material outputs final color through a separate fragment
// entry, not a change to this one.
//
// Color-space contract: albedo is stored sRGB-encoded (RGBA8Srgb) and sampled as
// linear — the sampler decodes on read, so a material writes its sampled albedo
// straight to G0 and the lighting pass reads it back in linear space for its
// N·L math. Normals are world-space, stored in a signed float format so the full
// [-1, 1] range survives without an encode/decode round-trip.
namespace Veng::Renderer
{
    // The g-buffer's two color attachments and its depth attachment. The
    // geometry pass renders into G0/G1 (+ depth); a fullscreen pass samples them.
    namespace GBuffer
    {
        // G0 — base color. rgb is the sRGB-encoded albedo; a is free (a material
        // flag channel).
        inline constexpr Format AlbedoFormat = Format::RGBA8Srgb;

        // G1 — world-space normal in xyz.
        inline constexpr Format NormalFormat = Format::RGBA16Sfloat;

        // The depth attachment is also the lighting pass's depth source, so it is
        // sampled as a texture downstream — the only depth target read as a
        // texture in the engine.
        inline constexpr Format DepthFormat = Format::D32Sfloat;

        // The color attachments are written by the geometry pass and sampled by a
        // downstream fullscreen pass; depth is a depth attachment also sampled
        // downstream. RenderGraph::Compile asserts a sampled target carries
        // Sampled, so the usage flags are load-bearing.
        inline constexpr ImageUsage ColorUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
        inline constexpr ImageUsage DepthUsage = ImageUsage::DepthAttachment | ImageUsage::Sampled;
    }
}
