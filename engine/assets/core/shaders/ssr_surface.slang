// The surface an SSR trace texel or composite pixel reflects from, taken on the pixel's unjittered
// ray. SSR composites after the temporal resolve, which reconstructs the scene at unjittered
// positions, so a reflection traced from the jittered g-buffer as it lies would follow the jitter.
//
// The g-buffer texel holding the pixel's unjittered position is the one the jitter moved it into
// (uv + JitterNdc / 2). That texel's surface was sampled at its own centre, up to a texel away from
// the pixel's ray, so its point is carried along its tangent plane onto the ray — exact for a plane
// at any angle, and with no temporal resolve (JitterNdc zero, the texel centred on the pixel) the
// texel's own point. Every read is a point load: a filtered depth or normal would blend the two
// surfaces either side of a silhouette into one that is neither.
#include "view_constants.slang"

// How far, in texel widths at the surface, a point may be carried along its tangent plane onto the
// pixel's ray. A plane seen nearly edge-on, or a silhouette where the normal turns away from the
// view, would carry it unboundedly; there the reflected ray runs almost along the view ray, so where
// along it the ray starts barely moves what it hits.
static const float MaxSurfaceCarryTexels = 16.0;

// The surface at one screen position.
struct SsrSurface
{
    float3 Position;  // camera-relative position, on the pixel's unjittered ray
    float3 Normal;    // world-space unit normal
    int2 Texel;       // the g-buffer texel it was read from, for the material reads beside it
    bool Valid;       // false over the cleared background or a texel with no normal
};

// The camera-relative position of a rasterized (jittered) screen UV at a device depth.
float3 RelativePosition(ViewConstants vc, float2 uv, float depth)
{
    float4 relativeH = mul(vc.InvViewRotProj, float4(uv * 2.0 - 1.0, depth, 1.0));
    return relativeH.xyz / relativeH.w;
}

// The surface at unjittered screen UV `uv`, from the g-buffer's depth and world-normal targets.
SsrSurface SsrSurfaceAt(ViewConstants vc, Texture2D depthTexture, Texture2D normalTexture, float2 uv)
{
    SsrSurface surface;
    surface.Position = float3(0.0, 0.0, 0.0);
    surface.Normal = float3(0.0, 0.0, 1.0);
    surface.Texel = int2(0, 0);
    surface.Valid = false;

    uint width;
    uint height;
    uint levels;
    depthTexture.GetDimensions(0u, width, height, levels);
    float2 size = float2(width, height);
    float2 rasterUV = uv + vc.JitterNdc.xy * 0.5;
    int2 texel = clamp(int2(floor(rasterUV * size)), int2(0, 0), int2(width, height) - 1);
    surface.Texel = texel;

    // Reverse-Z: the cleared background sits at depth 0.
    float depth = depthTexture.Load(int3(texel, 0)).r;
    if (depth <= 0.0)
        return surface;
    float3 normal = normalTexture.Load(int3(texel, 0)).xyz;
    float normalLength = length(normal);
    if (normalLength < 1e-4)
        return surface;
    surface.Normal = normal / normalLength;

    float2 texelUV = (float2(texel) + 0.5) / size;
    float3 sampled = RelativePosition(vc, texelUV, depth);
    // The pixel's ray, from its point at the texel's depth toward one further away, so a
    // perspective and an orthographic projection give it alike.
    float3 onRay = RelativePosition(vc, rasterUV, depth);
    float3 direction = RelativePosition(vc, rasterUV, depth * 0.5) - onRay;
    float directionLength = max(length(direction), 1e-20);
    direction /= directionLength;
    float carry = 0.0;
    float facing = dot(surface.Normal, direction);
    if (abs(facing) > 1e-6)
        carry = dot(surface.Normal, sampled - onRay) / facing;
    float texelWidth = length(RelativePosition(vc, texelUV + float2(1.0 / size.x, 0.0), depth) - sampled);
    float limit = MaxSurfaceCarryTexels * texelWidth;
    surface.Position = onRay + direction * clamp(carry, -limit, limit);
    surface.Valid = true;
    return surface;
}
