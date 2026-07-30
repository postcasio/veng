// The engine's GuiFill material contract: the set-0 bindless declarations a UI fill reads, the
// reserved GUI push block, the gui vertex stage's interpolants, and the fixed rounded-rect
// coverage a fill is resolved through. A GuiFill material (the engine's gui vertex stage, one
// premultiplied SV_Target0 color, drawn per UI quad by the GUI pass) `#include`s this and declares
// its own MaterialParams beside it — the parameter block is per-shader by definition (the cooker
// reflects each shader's own struct to pack its fields at the reflected offsets), so it is not
// part of the engine contract. The Surface, PostProcess, and Sky domains have their own contracts
// in Veng/surface.slang, Veng/postprocess.slang, and Veng/sky.slang.
//
// A material is a *fill source*, not a silhouette: it emits an RGBA that GuiFillResolve multiplies
// by the same rounded-rect signed-distance coverage and border ring the fixed shape path computes,
// so corner radius, border, clip, and rotation compose with a material fill exactly as they do
// with a flat color or a gradient.

// The per-material block byte stride. Mirrors BindlessRegistry::MaterialParamStride, and moves only with it; one stride is
// shared across every material so a single ByteAddressBuffer holds a different per-shader
// parameter layout, read at index * MaterialParamStride.
static const uint MaterialParamStride = 384;

// The set-0 bindless arrays a UI fill reads: the sampled images + samplers, the byte-address
// storage-buffer array, and the per-material param block. Mirrors the BindlessRegistry layout.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(3, 0)]] ByteAddressBuffer g_Buffers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

// The GUI pass's own push block, reserved verbatim by this domain, with the per-draw material
// selector placed immediately after it. The leading members are GuiScenePass's GuiPushConstants
// (the gui vertex stage reads InvScreenSize from the identical offsets, so one range serves both
// stages); MaterialIndex sits at byte offset 20, which is Veng::GuiFillSelectorPushOffset and what
// MaterialLoader's SelectorPushOffsetFor requires a merged range to cover.
struct GuiFillPushConstants
{
    float2 InvScreenSize; // 1 / UI image extent in pixels (read by the gui vertex stage)
    uint GradientBuffer;  // bindless g_Buffers slot of the gradient-record buffer
    uint GradientBase;    // record-index base for the current frame-in-flight
    float Time;           // seconds, for an animated fill
    uint MaterialIndex;   // per-material param-block selector (frame-folded)
};

[[vk::push_constant]] ConstantBuffer<GuiFillPushConstants> g_PC;

// The gui vertex stage's interpolants a fill reads, written by gui.vert. Semantics — not member
// order — bind these to the vertex outputs, so the gradient selector, shadow, and UV-wrap lanes a
// fill never reads are simply not declared here, as the MSDF text path likewise omits them. A
// material fill is not a sliced texture fill: it samples through its own declared parameters, so
// the per-cell wrap the nine-slice path needs has nothing to act on. That omission is the
// established convention in this domain, not an oversight.
struct GuiFillInputs
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float4 v_Color : COLOR0;
    float2 v_RectHalf : TEXCOORD1;
    float2 v_RectCoord : TEXCOORD2;
    nointerpolation float4 v_Params : TEXCOORD3;
};

// The fragment's position within the shape as a normalized box coordinate: -1 at one edge, +1 at
// the other, 0 at the center — the same space the gradient fill reduces to a ramp offset.
float2 GuiFillBoxCoord(GuiFillInputs input)
{
    return input.v_RectCoord / max(input.v_RectHalf, float2(1e-4, 1e-4));
}

// Signed distance to a rounded rectangle centered at the origin, half-extent b, corner radius r
// (subtracted from the box so the corners round inward).
float GuiFillRoundedBoxSdf(float2 p, float2 b, float r)
{
    float2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Resolves an authored fill into the pass's premultiplied linear output: the fixed rounded-rect
// coverage (screen-derivative anti-aliased, its width taken from the distance field's rate of
// change so the edge is resolution-independent) times the fill's own alpha, with a positive border
// width restricting the fill to the ring within `border` pixels of the edge. The silhouette is the
// engine's; a material never widens or replaces it.
float4 GuiFillResolve(GuiFillInputs input, float4 fill)
{
    float radius = input.v_Params.x;
    float border = input.v_Params.y;

    float dist = GuiFillRoundedBoxSdf(input.v_RectCoord, input.v_RectHalf, radius);
    float aa = max(fwidth(dist), 1e-4);
    float coverage = 1.0 - smoothstep(-aa, aa, dist);

    if (border > 0.0)
    {
        float innerCoverage = 1.0 - smoothstep(-aa, aa, dist + border);
        coverage *= 1.0 - innerCoverage;
    }

    float alpha = fill.a * coverage;
    return float4(fill.rgb * alpha, alpha);
}
