// Flipbook sprite vertex stage. Expands each sprite record (the per-frame SSBO at set 3 binding 0)
// into a camera-facing quad: six vertices selected by the vertex index, the corners placed about
// the sprite's anchor along the camera's right/up axes (the View matrix's first two rows), rolled
// in the screen plane, and projected through the jittered Proj so sprites resolve under TAA with
// the scene. Outputs the frame's atlas UV and the record's tint, texture, and alpha mode.
#include "view_constants.slang"

// One sprite record. Mirrors the CPU GpuSprite the pass uploads (std430, 80 bytes).
struct GpuSprite
{
    float4 PositionWidth; // xyz the world anchor, w the quad width
    float4 Color;         // rgb linear HDR tint, a opacity
    float4 UvRect;        // u0 v0 u1 v1, (u0, v0) the frame's top-left
    float2 Anchor;        // the anchor within the quad, x right, y up
    float Height;         // the quad height
    float Rotation;       // screen-plane roll, radians counter-clockwise
    uint Texture;         // atlas bindless index
    uint Sampler;         // atlas sampler bindless index
    uint AlphaMode;       // 0 premultiplied, 1 coverage, 2 luminance, 3 opaque
    uint Pad;
};

[[vk::binding(0, 3)]] StructuredBuffer<GpuSprite> g_Sprites;

struct PushConstants
{
    uint ViewConstantsIndex;
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float4 v_Color : COLOR0;
    nointerpolation uint v_Texture : TEXCOORD1;
    nointerpolation uint v_Sampler : TEXCOORD2;
    nointerpolation uint v_AlphaMode : TEXCOORD3;
};

[shader("vertex")]
VSOutput vsMain()
{
    // The VertexIndex builtin read directly (it includes the draw's firstVertex, which selects the
    // alpha or additive run); SV_VertexID would pull in the DrawParameters capability.
    uint vertexId = uint(spirv_asm { result:$$int = OpLoad builtin(VertexIndex:int); });
    GpuSprite sprite = g_Sprites[vertexId / 6];
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    // Two-triangle quad corners in [0, 1]^2, (0, 0) the bottom-left.
    static const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
        float2(1.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    float2 corner = corners[vertexId % 6];

    // The corner's offset from the anchor in quad space, then rolled in the screen plane.
    float2 local = (corner - sprite.Anchor) * float2(sprite.PositionWidth.w, sprite.Height);
    float s = sin(sprite.Rotation);
    float c = cos(sprite.Rotation);
    float2 rolled = float2(c * local.x - s * local.y, s * local.x + c * local.y);

    float3 right = float3(view.View[0][0], view.View[0][1], view.View[0][2]);
    float3 up = float3(view.View[1][0], view.View[1][1], view.View[1][2]);
    float4 worldPos =
        float4(sprite.PositionWidth.xyz + rolled.x * right + rolled.y * up, 1.0);

    VSOutput output;
    output.sv_position = mul(view.Proj, mul(view.View, worldPos));
    // The atlas's v runs top to bottom; the quad's y runs bottom to top.
    output.v_UV = float2(lerp(sprite.UvRect.x, sprite.UvRect.z, corner.x),
                         lerp(sprite.UvRect.w, sprite.UvRect.y, corner.y));
    output.v_Color = sprite.Color;
    output.v_Texture = sprite.Texture;
    output.v_Sampler = sprite.Sampler;
    output.v_AlphaMode = sprite.AlphaMode;
    return output;
}
