// Particle vertex stage. Expands each drawn particle into a quad of six vertices selected by the
// vertex index over the list the push names — the sorted alpha keys or the additive list — placed
// in view space from the record's eye-relative centre (rotated by the View matrix's rotation
// alone, so no large coordinate meets the view translation) and projected through the jittered
// Proj, so particles resolve under TAA with the scene.
//
// A billboard's corners sit about its flipbook's anchor along the view's x and y, rolled in the
// screen plane and sized by the blended frame extent, as a sprite's. A streak spans from its tail
// to its head, its width across the projected axis, floored at MinPixelWidth with its coverage
// scaled by the ratio; one whose axis is shorter than its width draws as a square about its head.
#include "view_constants.slang"
#include "particle_draw.slang"

// The narrowest a streak draws, in pixels, as a ribbon's.
static const float MinPixelWidth = 1.5;

float3 ToView(ViewConstants view, float3 v)
{
    return float3(dot(view.View[0].xyz, v), dot(view.View[1].xyz, v), dot(view.View[2].xyz, v));
}

[shader("vertex")]
VSOutput vsMain(uint vertexId : SV_VulkanVertexID)
{
    uint index = vertexId / 6u;
    uint drawIndex = g_PC.List == 0u ? g_Set3.AlphaKeys[index].y : g_Set3.AdditiveList[index];
    GpuParticleDraw draw = g_Set3.Draws[drawIndex];
    GpuParticleEmitter emitter = g_Set3.Emitters[draw.Emitter];
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    static const float2 corners[6] = {
        float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0),
        float2(1.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0),
    };
    float2 corner = corners[vertexId % 6u];
    float3 head = ToView(view, draw.CentreWidth.xyz);
    float width = draw.CentreWidth.w;

    VSOutput output;
    output.v_Draw = drawIndex;
    output.v_Coverage = 1.0;
    output.v_Round = 0u;
    float3 position;
    if (emitter.Mode.x == ParticleRenderStreak)
    {
        float3 tail = ToView(view, draw.CentreWidth.xyz + draw.TailHeight.xyz);
        float4 clipHead = mul(view.Proj, float4(head, 1.0));
        float worldPerPixel = 2.0 * abs(clipHead.w) / (abs(view.Proj[1][1]) * view.ExtentParams.y);
        float halfWidth = max(0.5 * width, 0.5 * MinPixelWidth * worldPerPixel);
        output.v_Coverage = halfWidth > 0.0 ? (0.5 * width) / halfWidth : 0.0;

        float3 axis = head - tail;
        float axisLength = length(axis);
        if (axisLength <= 2.0 * halfWidth)
        {
            // Too short to read as a streak: a camera-facing square about the head.
            float2 profile = corner * 2.0 - 1.0;
            position = head + float3(profile * halfWidth, 0.0);
            output.v_Quad = profile;
            output.v_Round = 1u;
        }
        else
        {
            float3 tangent = axis / axisLength;
            float3 side = cross(tangent, corner.x > 0.5 ? head : tail);
            if (dot(side, side) < 1e-12)
            {
                side = cross(tangent, abs(tangent.y) < 0.9 ? float3(0.0, 1.0, 0.0)
                                                           : float3(1.0, 0.0, 0.0));
            }
            side = normalize(side);
            float across = corner.y * 2.0 - 1.0;
            position = (corner.x > 0.5 ? head : tail) + side * (across * halfWidth);
            output.v_Quad = float2(corner.x, across);
        }
    }
    else
    {
        SpriteFramePair pair = ParticleFramePair(emitter, draw);
        float extent = SpriteFrameExtent(pair);
        float2 size = float2(width, draw.TailHeight.w);
        float2 local = ((corner - 0.5) * extent + 0.5 - emitter.Frame.xy) * size;
        float s = sin(draw.Rotation);
        float c = cos(draw.Rotation);
        float2 rolled = float2(c * local.x - s * local.y, s * local.x + c * local.y);
        position = head + float3(rolled, 0.0);
        output.v_Quad = float2(corner.x, 1.0 - corner.y);
    }

    output.sv_position = mul(view.Proj, float4(position, 1.0));
    output.v_ViewDepth = -position.z;
    return output;
}
