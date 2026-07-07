// GUI shape fragment path: a rounded-rectangle signed-distance field with
// screen-derivative anti-aliased edges and an optional inner border, its fill one of
// three sources — a flat vertex color, a bindless texture modulating the color (a
// plain textured quad or one 9-slice tile is a zero-radius rect with a bound texture),
// or a gradient that reduces the fragment's normalized box position to an offset into a
// 1D ramp LUT (multi-stop color baked into the ramp). The interpolated per-fragment
// local coordinate and the rect half-extent reconstruct the SDF: negative inside, zero
// on the edge. Corner radius and border width ride the params; the texture and sampler
// indices are bindless set-0 slots, with a negative texture index meaning untextured.
// Output is premultiplied linear RGBA so the pass blends it "over" the scene in linear
// space.

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_Uv : TEXCOORD0;
    float4 v_Color : COLOR0;
    float2 v_RectHalf : TEXCOORD1;
    float2 v_RectCoord : TEXCOORD2;
    nointerpolation float4 v_Params : TEXCOORD3;
    nointerpolation uint v_GradientSelector : TEXCOORD4;
};

// Must match GuiScenePass's push block and the vertex stage: the gradient-record buffer's bindless
// slot and the current frame-in-flight's record base.
struct PushConstants
{
    float2 InvScreenSize;
    uint GradientBuffer;
    uint GradientBase;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// The GPU gradient record, matching Gui::GpuGradient byte-for-byte (48 bytes, scalar layout). Loaded
// from the byte-address gradient buffer by index; geometry is in normalized box space.
struct GpuGradient
{
    uint Kind; // 0 linear, 1 radial, 2 conic
    uint RampTexture;
    uint RampSampler;
    uint Pad0;
    float2 P0; // linear start / radial + conic center
    float2 P1; // linear end / radial radii
    float AngleOffset;
    float Pad1;
    float Pad2;
    float Pad3;
};

static const float InvTau = 0.15915494309189535; // 1 / (2*pi)
static const uint GradientStride = 48;

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(3, 0)]] ByteAddressBuffer g_Buffers[];

// Signed distance to a rounded rectangle centered at the origin, half-extent b,
// corner radius r (subtracted from the box so the corners round inward).
float RoundedBoxSdf(float2 p, float2 b, float r)
{
    float2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    float radius = input.v_Params.x;
    float border = input.v_Params.y;
    int textureIndex = int(input.v_Params.z);
    int samplerIndex = int(input.v_Params.w);

    // v_GradientSelector is 0 for no gradient, else the record index plus one. A gradient loads its
    // record from the byte-address gradient buffer, reduces the fragment's normalized box position to
    // a ramp offset t, and samples the record's ramp; a plain textured quad samples by UV instead.
    uint selector = input.v_GradientSelector;
    float4 color = input.v_Color;
    if (selector > 0)
    {
        GpuGradient grad = g_Buffers[NonUniformResourceIndex(g_PC.GradientBuffer)]
                               .Load<GpuGradient>((g_PC.GradientBase + selector - 1) * GradientStride);
        float2 p = input.v_RectCoord / max(input.v_RectHalf, float2(1e-4, 1e-4));
        float t;
        if (grad.Kind == 0)
        {
            float2 axis = grad.P1 - grad.P0;
            t = dot(p - grad.P0, axis) / max(dot(axis, axis), 1e-6);
        }
        else if (grad.Kind == 1)
        {
            t = length((p - grad.P0) / max(grad.P1, float2(1e-6, 1e-6)));
        }
        else
        {
            t = frac(atan2(p.y - grad.P0.y, p.x - grad.P0.x) * InvTau - grad.AngleOffset);
        }
        float4 ramp = g_Textures[NonUniformResourceIndex(grad.RampTexture)].Sample(
            g_Samplers[NonUniformResourceIndex(grad.RampSampler)], float2(saturate(t), 0.5));
        color *= ramp;
    }
    else if (textureIndex >= 0)
    {
        float4 texel = g_Textures[NonUniformResourceIndex(textureIndex)].Sample(
            g_Samplers[NonUniformResourceIndex(samplerIndex)], input.v_Uv);
        color *= texel;
    }

    // Screen-space coverage from the SDF: a one-pixel-wide transition centered on
    // the edge, its width taken from the distance field's rate of change so the
    // anti-aliasing is resolution-independent.
    float dist = RoundedBoxSdf(input.v_RectCoord, input.v_RectHalf, radius);
    float aa = max(fwidth(dist), 1e-4);
    float coverage = 1.0 - smoothstep(-aa, aa, dist);

    // A positive border width fills only the ring within `border` pixels of the
    // edge, leaving the interior transparent; zero border fills the whole shape.
    if (border > 0.0)
    {
        float inner = RoundedBoxSdf(input.v_RectCoord, input.v_RectHalf, radius) + border;
        float innerCoverage = 1.0 - smoothstep(-aa, aa, inner);
        coverage *= 1.0 - innerCoverage;
    }

    float alpha = color.a * coverage;
    return float4(color.rgb * alpha, alpha);
}
