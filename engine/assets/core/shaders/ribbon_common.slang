// The ribbon stages' shared push block and interpolants. Both placements push the same block; the
// scene entries read only ViewConstantsIndex, the post-resolve entries the rest as well. Mirrors the
// CPU RibbonPushConstants byte for byte.
struct PushConstants
{
    float4x4 Proj;           // post-resolve: the unjittered view -> clip projection
    uint ViewConstantsIndex; // the current frame-in-flight's view-constants region
    uint DepthTexture;       // post-resolve: the render-allocation g-buffer depth's bindless index
    uint Sampler;            // post-resolve: the sampler the depth is read through
    uint Occluded;           // post-resolve: 1 to discard fragments behind the scene depth, 0 not to
    float2 TargetExtent;     // post-resolve: the post-resolve allocation's extent, in pixels
    float2 Pad1;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float4 v_Color : COLOR0;
    // Where the fragment lies in the record's footprint, 1 at its rim: a band's (across, 0), a dot's
    // disc coordinate.
    float2 v_Profile : TEXCOORD0;
    // The fragment's distance along the view axis, which the post-resolve occlusion compares.
    float v_ViewDepth : TEXCOORD1;
};
