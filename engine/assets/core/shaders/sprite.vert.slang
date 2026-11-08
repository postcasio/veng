// Flipbook sprite vertex stage. Expands each sprite record (the per-frame SSBO at set 3 binding 0)
// into a camera-facing quad: six vertices selected by the vertex index, the corners placed about
// the sprite's anchor along the camera's right/up axes (the View matrix's first two rows), rolled
// in the screen plane, and projected through the jittered Proj so sprites resolve under TAA with
// the scene. A flipbook whose frames differ in extent sizes the quad by the blended extent about
// the frame centre. A six-way sprite gathers the scene's light here, once per sprite: every vertex
// of a sprite computes it from identical inputs, and it reaches the fragment stage flat.
#include "view_constants.slang"

#include "Veng/bindless.slang"

#include "sprite_record.slang"
#include "Veng/sprite_shading.slang"

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float4 v_Color : COLOR0;
    nointerpolation uint v_Texture : TEXCOORD1;
    nointerpolation uint v_Sampler : TEXCOORD2;
    nointerpolation uint v_AlphaMode : TEXCOORD3;
    nointerpolation uint v_Sprite : TEXCOORD4;
    // The quad's frame UV: u right, v down, (0.5, 0.5) the frame centre.
    float2 v_Quad : TEXCOORD5;
    // The positive view-space depth, exact under perspective-correct interpolation of a flat quad.
    float v_ViewDepth : TEXCOORD6;
    nointerpolation float3 v_In0 : TEXCOORD7;
    nointerpolation float3 v_In1 : TEXCOORD8;
    nointerpolation float3 v_In2 : TEXCOORD9;
    nointerpolation float3 v_In3 : TEXCOORD10;
    nointerpolation float3 v_In4 : TEXCOORD11;
    nointerpolation float3 v_In5 : TEXCOORD12;
    nointerpolation float3 v_Ambient : TEXCOORD13;
};

// SV_VulkanVertexID includes the draw's firstVertex, which selects the alpha or additive run.
[shader("vertex")]
VSOutput vsMain(uint vertexId : SV_VulkanVertexID)
{
    uint spriteIndex = vertexId / 6;
    GpuSprite sprite = g_Set3.Sprites[spriteIndex];
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    // Two-triangle quad corners in [0, 1]^2, (0, 0) the bottom-left.
    static const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
        float2(1.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    float2 corner = corners[vertexId % 6];

    // The corner's offset from the anchor in quad space, scaled about the frame centre by the
    // blended extent (exactly the unscaled corner at an extent of 1), then rolled in the screen
    // plane.
    float extent = SpriteFrameExtent(SpriteFramePairOf(sprite));
    float2 size = float2(sprite.PositionWidth.w, sprite.Height);
    float2 local = ((corner - 0.5) * extent + 0.5 - sprite.Anchor) * size;
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
    output.v_Sprite = spriteIndex;
    output.v_Quad = float2(corner.x, 1.0 - corner.y);
    output.v_ViewDepth = -mul(view.View, worldPos).z;

    SpriteLightGather gather = SpriteLightGatherNone();
    if ((sprite.Flags & SpriteFlagSixWay) != 0u)
    {
        // The sprite's frame is the camera's rolled right and up, Z = X × Y toward the viewer; its
        // centre is the frame centre, offset from the anchor by the same roll.
        float3 axisX = c * right + s * up;
        float3 axisY = -s * right + c * up;
        float3 axisZ = cross(axisX, axisY);
        float2 toCentre = (0.5 - sprite.Anchor) * size;
        float3 centre = sprite.PositionWidth.xyz + toCentre.x * axisX + toCentre.y * axisY;
        float radius = 0.5 * max(size.x, size.y) * extent;
        gather = EvaluateSpriteLighting(view, centre, axisX, axisY, axisZ, radius);
    }
    output.v_In0 = gather.In[0];
    output.v_In1 = gather.In[1];
    output.v_In2 = gather.In[2];
    output.v_In3 = gather.In[3];
    output.v_In4 = gather.In[4];
    output.v_In5 = gather.In[5];
    output.v_Ambient = gather.Ambient;
    return output;
}
