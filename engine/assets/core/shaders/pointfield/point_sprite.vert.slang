// Point-field resolved-sprite vertex stage. The per-point sprite work (projection, pixel-size
// clamp, flux gain, color unpack, opacity fold) runs once per point in the expansion compute pass
// (point_sprite_expand.comp), which emits one compact GpuSpriteRecord per surviving point into the
// record buffer at set 1 binding 0. This stage collapses to a record fetch plus one corner FMA in
// clip space: four unique vertices per point, indexed as two triangles by the pass's shared quad
// index buffer, drawn by one DrawIndexedIndirect over the compacted survivors.

// One compacted sprite record. 32 bytes; matches point_sprite_expand.comp and the CPU
// GpuSpriteRecord byte-for-byte.
struct GpuSpriteRecord
{
    float4 ClipCenter; // projected sprite center (pre-divide)
    uint2 HalfExtents; // clip-space half-extent xy: right offset (2×f16), up offset (2×f16)
    uint2 Color;       // gain·opacity-folded HDR color: rg (2×f16), b + occluded-fade flag (2×f16)
};

[[vk::binding(0, 1)]] StructuredBuffer<GpuSpriteRecord> g_Records;

struct PushConstants
{
    uint DepthTexture;
    uint Sampler;
    uint ViewConstantsIndex;
    uint FirstPoint;
    uint PointCount;
    float MinPixels;
    float MaxPixels;
    float MaxIntensity;
    float ViewportWidth;
    float ViewportHeight;
    float Opacity;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float4 v_Color : COLOR0;
    float2 v_ScreenUV : TEXCOORD1;
    float v_NdcDepth : TEXCOORD2;
};

// Unpacks a uint holding a pair of IEEE-754 half-floats (x low 16 bits, y high) into two floats.
float2 UnpackHalf2(uint packed)
{
    return float2(f16tof32(packed & 0xFFFFu), f16tof32(packed >> 16));
}

[shader("vertex")]
VSOutput vsMain()
{
    // Read the VertexIndex builtin directly (not SV_VertexID) to avoid the base-vertex subtract
    // that pulls in the DrawParameters capability the device does not enable. The indexed draw
    // sets firstIndex=0 and vertexOffset=0, so the builtin equals the index buffer's value.
    uint vertexId = uint(spirv_asm { result:$$int = OpLoad builtin(VertexIndex:int); });

    // An indexed draw of 4 unique vertices per surviving sprite (vertex id = 4*record + corner):
    // vertexId / 4 selects the record, vertexId & 3 the quad corner. The shared index buffer
    // expands the four corners into two triangles, so the two shared corners hit the post-transform
    // cache instead of re-shading.
    uint recordId = vertexId / 4;
    uint cornerId = vertexId & 3;

    GpuSpriteRecord record = g_Records[recordId];

    // The four quad corners in [-1, 1], indexed by cornerId. The record's half-extents already fold
    // the point's own 0.5 half-size, so the corner sign selects the pre-scaled offset directly.
    static const float2 corners[4] = {
        float2(-1.0, -1.0),
        float2(1.0, -1.0),
        float2(-1.0, 1.0),
        float2(1.0, 1.0),
    };
    float2 corner = corners[cornerId];

    float2 halfRight = UnpackHalf2(record.HalfExtents.x);
    float2 halfUp = UnpackHalf2(record.HalfExtents.y);

    float2 rg = UnpackHalf2(record.Color.x);
    float2 bFade = UnpackHalf2(record.Color.y);
    float4 color = float4(rg, bFade.x, 1.0);

    // The one FMA: offset the projected center's clip-space xy by the corner-selected half-extents.
    // The z/w inherit clipCenter (a screen-facing sprite's per-corner depth skew is below the f16
    // record precision), so the sprite composites additively at the point's own depth.
    float4 clip = record.ClipCenter;
    clip.xy += corner.x * halfRight + corner.y * halfUp;

    VSOutput output;
    output.sv_position = clip;
    output.v_UV = corner * 0.5;
    output.v_Color = color;
    output.v_ScreenUV = (clip.xy / clip.w) * 0.5 + 0.5;
    output.v_NdcDepth = clip.z / clip.w;
    return output;
}
