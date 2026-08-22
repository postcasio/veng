#pragma once

#include <array>

#include <Veng/Veng.h>
#include <Veng/Math/SphericalHarmonics.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/ShadowCascades.h>

namespace Veng::Renderer
{
    // The per-frame view-constants block (set-0 binding 5): camera/view state only.
    // The directional shadow system rides the set-1 ShadowConstants block.
    // Mirrors view_constants.slang ViewConstants byte-for-byte.
    struct ViewConstantsBlock
    {
        mat4 InvViewProj; // world-position reconstruction from depth (jittered under TAA)
        // Inverse of (Proj x rotation-only View): a translation-free view-direction reconstruction.
        // Deriving a direction from InvViewProj instead cancels two large near-equal world points,
        // which loses f32 precision with distance from the world origin; this carries no translation
        // to cancel. Jittered with Proj, so it matches what was rasterized.
        mat4 InvViewRotProj;
        vec4 CameraPosition; // xyz; w unused
        mat4 View;           // world → view (the SSAO pass reconstructs view space)
        mat4 Proj;           // view → clip (jittered under TAA)
        mat4 PrevViewProj;   // previous frame's unjittered world → clip (TAA reprojection)
        mat4 CurViewProj;    // this frame's unjittered world → clip (TAA velocity)
        vec4 RenderScaleUV;  // xy this frame's validExtent/allocExtent, zw previous frame's
        vec4 MaxValidUV;     // xy this frame's (validExtent-0.5)/allocExtent, zw previous
        // Cosine-convolved sky irradiance SH (9 RGB coefficients, .xyz per element, .w pad).
        // vec4[9] (not vec3[9]) to share one 16-byte std140 stride with the shader.
        std::array<vec4, ShCoefficientCount> SkyShCoeffs;
        vec4 TimeParams;   // x seconds since engine start (frame-locked), y frame delta
        vec4 ExtentParams; // xy valid (sub-rect) extent px, zw allocation extent px
        // x refraction scene-color texture handle, y sampler handle, z 1 when the copy
        // pass runs this frame (Settings.Refraction), w the opaque-depth copy's texture handle.
        uvec4 SceneColor;
        // x the number of mip levels the scene-color grab carries — 1 when Settings.RefractionBlur
        // is off, so a blurred sample degrades to the sharp one rather than reading a level that
        // does not exist. yzw unused.
        uvec4 SceneColorChain;
    };

    static_assert(sizeof(ViewConstantsBlock) <= BindlessRegistry::ViewConstantsStride,
                  "ViewConstantsBlock must fit one ring-buffered view-constants region");

    // One cascade set: one near-parallel light's cascades, fit to that light's direction.
    // std140: ViewProj is float4x4[MaxCascades] (16-byte aligned elements) and each per-cascade
    // scalar array rides one vec4 (avoiding per-element std140 padding). 304 bytes, a multiple
    // of 16, so an array of these is contiguous with no inter-element padding.
    struct CascadeSetBlock
    {
        mat4 ViewProj[MaxCascades]; // 256 — tile-remap baked in (for the sample)
        vec4 Splits;                // 16  — per-cascade view-space far distance
        vec4 TexelSize;             // 16  — per-cascade world units per shadow texel
        vec4 DepthRange;            // 16  — per-cascade render ortho depth extent, world units
    };

    static_assert(sizeof(CascadeSetBlock) == 304,
                  "CascadeSetBlock must be the std140-packed 304-byte set record");

    // The directional-shadow constants (set 1 binding 2, ring-buffered dynamic uniform).
    // Mirrors shadow.slang's ShadowConstants byte-for-byte.
    struct ShadowConstantsBlock
    {
        CascadeSetBlock Sets[MaxCascadeSets];
        vec4 ShadowParams; // x 1/tileRes, y blend-band, z cascade count, w enabled
    };

    static_assert(sizeof(ShadowConstantsBlock) == MaxCascadeSets * 304 + 16,
                  "ShadowConstantsBlock must be the std140-packed cascade-set array plus the "
                  "trailing ShadowParams vec4");

    // Set 1 binding 3, ring-buffered dynamic uniform. Separate from ShadowConstantsBlock
    // so the directional block's layout is unchanged when punctual records are added.
    struct PunctualShadowBlock
    {
        PunctualShadowRecord Records[MaxShadowedPunctual];
        vec4 AtlasParams; // x 1/tileRes (per-tile texel size), yzw pad
    };

    static_assert(sizeof(PunctualShadowBlock) == MaxShadowedPunctual * 416 + 16,
                  "PunctualShadowBlock must be the std140-packed record array (416 bytes per "
                  "record) plus the trailing AtlasParams vec4");

    // Per-draw record indexed by the candidate id; std430-identical to the shader's
    // DrawData. World is the model matrix; the three NormalColumns carry the
    // inverse-transpose of its upper 3×3; MaterialIndex is the frame-folded selector.
    struct GpuDrawData
    {
        mat4 World;
        vec4 NormalColumn0;
        vec4 NormalColumn1;
        vec4 NormalColumn2;
        u32 MaterialIndex;
        u32 PaletteBase;
        u32 PrevPaletteBase;
        u32 EntityIndex; // packed Entity slot index; the picking fragment writes index + 1
        mat4 PrevWorld;
    };

    static_assert(sizeof(GpuDrawData) == 192,
                  "GpuDrawData must match the shader DrawData (192 bytes)");

    // One uploaded camera-frustum survivor; std430-identical to the cull shader's
    // CullCandidate: world AABB plus the indexed-draw args its command needs.
    struct GpuCullCandidate
    {
        vec4 BoundsMin;
        vec4 BoundsMax;
        u32 IndexCount;
        u32 FirstIndex;
        i32 VertexOffset;
        u32 FirstInstance;
    };

    static_assert(sizeof(GpuCullCandidate) == 48,
                  "GpuCullCandidate must match the shader CullCandidate (48 bytes)");

    // VkDrawIndexedIndirectCommand laid out by hand (20 bytes), the stride the
    // indirect geometry pass issues over.
    struct DrawIndexedIndirectCommand
    {
        u32 IndexCount;
        u32 InstanceCount;
        u32 FirstIndex;
        i32 VertexOffset;
        u32 FirstInstance;
    };

    static_assert(sizeof(DrawIndexedIndirectCommand) == 20,
                  "DrawIndexedIndirectCommand must match VkDrawIndexedIndirectCommand");

    // The surface push block (vertex stage), matching surface.slang PushConstants:
    // FrameBase folds the ring-buffered DrawData region into the candidate id; the
    // view-constants index selects the per-frame set-0 view block the vertex stage
    // multiplies by. Both cull modes push the same block.
    struct SurfacePush
    {
        u32 FrameBase;
        u32 ViewConstantsIndex;
    };
}
