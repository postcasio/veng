// Geometric specular anti-aliasing: how far a shading normal varies across a pixel, and how far
// that widens the GGX lobe (Kaplanyan 2016; Tokuyoshi and Kaplanyan 2019, as Filament's
// normalFiltering ships it):
//     alpha² += min(2 · σ² · variance, κ)
// with σ² and κ the view block's AmbientParams.y and .z; a zero σ² adds exactly nothing. A highlight
// narrower than the normal's variation across one pixel lands on it one frame and misses it the
// next; the widened lobe covers the pixel. The lighting core widens every surface it shades once
// (MakeLightingSurface), and screen-space reflections widen the cone they reflect by the same law,
// so a pixel reflects the lobe it is shaded with.
//
// The variance is |∂N/∂x|² + |∂N/∂y|² in fine derivatives. A pass reading the g-buffer takes it
// either way: GBufferNormalVariance with derivatives, in uniform control flow, at the g-buffer's
// own pixel grid; GBufferNormalVarianceAt from loads, anywhere and at any resolution, reproducing
// it exactly for the texel it names.
//
// The includer declares the view block (view_constants.slang) before including this.

#ifndef VENG_SPECULAR_AA_SLANG
#define VENG_SPECULAR_AA_SLANG

#include "normal_variance.slang"

// How far a shading normal varies across this pixel, |dN/dx|² + |dN/dy|². It takes screen-space
// derivatives, so it is called from a fragment stage in uniform control flow. Fine derivatives, so
// a pixel's variance comes from its own row and column neighbours.
float NormalScreenVariance(float3 normal)
{
    float3 du = ddx_fine(normal);
    float3 dv = ddy_fine(normal);
    return dot(du, du) + dot(dv, dv);
}

// The alpha² geometric specular anti-aliasing adds for a normal variance: min(2 · σ² · variance, κ).
float GeometricSpecularWidening(ViewConstants vc, float normalVariance)
{
    return min(2.0 * vc.AmbientParams.y * normalVariance, vc.AmbientParams.z);
}

// A perceptual roughness widened by a normal variance's geometric term. A zero widening — a flat
// surface, or the feature off — returns the roughness untouched.
float WidenRoughnessForGeometry(ViewConstants vc, float roughness, float normalVariance)
{
    float widening = GeometricSpecularWidening(vc, normalVariance);
    return WidenRoughnessByNormalVariance(roughness, widening, 1.0);
}

// The g-buffer normal's screen-space variance, or zero for a pixel whose 2×2 quad holds a background
// pixel. The cleared g-buffer's zero normal would otherwise read as a huge variance and roughen a
// silhouette against the sky. A quad straddling two different surfaces does read their normal
// difference as variance, so those edge pixels roughen, bounded by κ. Takes derivatives, so it runs
// in uniform control flow, at the g-buffer's own resolution.
float GBufferNormalVariance(float3 normal, bool hasSurface)
{
    float surface = hasSurface ? 1.0 : 0.0;
    // Fine derivatives pair a pixel with its row and column neighbours only; a second derivative of
    // that edge reaches the diagonal one, so any background pixel in the quad is seen by all four.
    float edge = abs(ddx_fine(surface)) + abs(ddy_fine(surface));
    float quadEdge = edge + abs(ddx_fine(edge)) + abs(ddy_fine(edge));
    float variance = NormalScreenVariance(normal);
    return quadEdge > 0.0 ? 0.0 : variance;
}

// The unit normal a g-buffer texel holds, or zero where it holds none. `texel` is clamped to the
// target, as a clamped sampler reads past its edge.
float3 GBufferUnitNormal(Texture2D normalTexture, int2 texel, int2 size)
{
    float3 normal = normalTexture.Load(int3(clamp(texel, int2(0, 0), size - 1), 0)).xyz;
    float normalLength = length(normal);
    return normalLength > 1e-4 ? normal / normalLength : float3(0.0, 0.0, 0.0);
}

// GBufferNormalVariance for g-buffer texel `texel`, from loads: the fine derivative pairs a texel
// with the other one in its 2×2 quad along each axis, and the guard zeroes it when any texel of the
// quad is background (reverse-Z depth 0). It needs no derivatives, so a pass running past an early
// return, or at a different resolution from the g-buffer, reads the value the deferred pass shaded
// that texel with.
float GBufferNormalVarianceAt(Texture2D depthTexture, Texture2D normalTexture, int2 texel)
{
    uint width;
    uint height;
    uint levels;
    normalTexture.GetDimensions(0u, width, height, levels);
    int2 size = int2(width, height);
    int2 quad = texel - (texel & 1);
    [unroll]
    for (int j = 0; j < 2; ++j)
    {
        [unroll]
        for (int i = 0; i < 2; ++i)
        {
            int2 corner = clamp(quad + int2(i, j), int2(0, 0), size - 1);
            if (depthTexture.Load(int3(corner, 0)).r <= 0.0)
                return 0.0;
        }
    }
    float3 normal = GBufferUnitNormal(normalTexture, texel, size);
    float3 du = GBufferUnitNormal(normalTexture, int2(texel.x ^ 1, texel.y), size) - normal;
    float3 dv = GBufferUnitNormal(normalTexture, int2(texel.x, texel.y ^ 1), size) - normal;
    return dot(du, du) + dot(dv, dv);
}

#endif
