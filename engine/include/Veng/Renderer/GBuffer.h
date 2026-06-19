#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

// The deferred g-buffer layout — the fixed contract every opaque material
// pipeline and the geometry pass agree on. A material's fragment shader writes
// these channels (through the GBufferOutput struct in the shader), the geometry
// pass clears and stores them, and the lighting pass samples them.
//
// The layout is albedo (G0) / world-normal (G1) / packed ORM (G2) plus a sampled
// depth attachment the lighting pass reconstructs world position from. It is the
// OPAQUE material contract — a transparent/forward material outputs final color
// through a separate fragment entry, not a change to this one.
//
// Color-space contract: albedo is stored sRGB-encoded (RGBA8Srgb) and sampled as
// linear — the sampler decodes on read, so a material writes its sampled albedo
// straight to G0 and the lighting pass reads it back in linear space for its BRDF
// math. Normals are world-space, stored in a signed float format so the full
// [-1, 1] range survives without an encode/decode round-trip. The ORM channels
// are perceptually fine at 8-bit unorm.
namespace Veng::Renderer
{
    // The g-buffer's three color attachments and its depth attachment. The
    // geometry pass renders into G0/G1/G2 (+ depth); a fullscreen pass samples them.
    namespace GBuffer
    {
        // G0 — base color. rgb is the sRGB-encoded albedo; a is unused (reserved).
        inline constexpr Format AlbedoFormat = Format::RGBA8Srgb;

        // G1 — world-space normal in xyz.
        inline constexpr Format NormalFormat = Format::RGBA16Sfloat;

        // G2 — packed material params. R=occlusion, G=roughness, B=metallic, A=emissive
        // strength. RGBA8: roughness/metallic/occlusion are perceptually fine at 8-bit;
        // emissive strength is a low-dynamic scalar (the lighting pass scales albedo by it).
        inline constexpr Format ORMFormat = Format::RGBA8Unorm;

        // The depth attachment is also the lighting pass's depth source, so it is
        // sampled as a texture downstream — the only depth target read as a
        // texture in the engine. The lighting pass reconstructs world position
        // from it via the camera's inverse view-projection.
        inline constexpr Format DepthFormat = Format::D32Sfloat;

        // The color attachments are written by the geometry pass and sampled by a
        // downstream fullscreen pass; depth is a depth attachment also sampled
        // downstream. RenderGraph::Compile asserts a sampled target carries
        // Sampled, so the usage flags are load-bearing.
        inline constexpr ImageUsage ColorUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
        inline constexpr ImageUsage DepthUsage = ImageUsage::DepthAttachment | ImageUsage::Sampled;
    }
}
