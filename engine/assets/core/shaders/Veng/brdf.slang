// The engine's microfacet BRDF terms, declared without bindings so a compute bake can include them
// as readily as a lighting pass: the GGX normal distribution, its importance sampler and the
// Hammersley point set that drives it, the two Smith-Schlick geometry terms, and Schlick Fresnel.
// The lighting core (Veng/lighting.slang) evaluates them per light, and the split-sum bakes
// (ibl_prefilter.comp, ibl_brdf_lut.comp) integrate them, so each term has one definition.
//
// Roughness: a material authors perceptual roughness r; the GGX width is alpha = r². The
// distribution and the sampler take alpha, so a caller that widens the lobe works in alpha
// directly. The geometry and roughness-aware Fresnel terms take r, since their fits are in r.

static const float BrdfPi = 3.14159265358979323846;

// GGX / Trowbridge-Reitz normal distribution D(h) for a GGX width alpha. nDotH is the caller's
// clamped N·H; N and H are unit.
//
// The textbook denominator (n·h)²(alpha² − 1) + 1 cancels catastrophically toward the peak, where
// it shrinks to alpha² while its terms stay near 1, so at a small alpha it loses most of its
// precision. Here 1 − (n·h)² is taken as |n × h|², which keeps full precision as h approaches n. For unit
// vectors the denominator is (1 − c²) + c²alpha² with c = N·H, so it is positive wherever alpha > 0
// and H is not opposite N; no clamp is applied, and the peak is the full 1 / (pi alpha²).
float BrdfDistributionGGX(float3 N, float3 H, float nDotH, float alpha)
{
    float3 nxh = cross(N, H);
    float oneMinusNoH2 = dot(nxh, nxh);
    float a = nDotH * alpha;
    float k = alpha / (oneMinusNoH2 + a * a);
    return k * k * (1.0 / BrdfPi);
}

// The Schlick-GGX geometry term for one direction, with the direct-lighting k = (r + 1)² / 8.
float BrdfGeometrySchlickGGX(float nDotX, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return nDotX / (nDotX * (1.0 - k) + k);
}

// Smith geometry over the view and light directions, direct-lighting k.
float BrdfGeometrySmith(float nDotV, float nDotL, float roughness)
{
    return BrdfGeometrySchlickGGX(nDotV, roughness) * BrdfGeometrySchlickGGX(nDotL, roughness);
}

// The Schlick-GGX geometry term for one direction, with the image-based k = r² / 2. The split-sum
// integration uses this k rather than the direct one, which is a fit for a single analytic light.
float BrdfGeometrySchlickGGXIbl(float nDotX, float roughness)
{
    float k = (roughness * roughness) / 2.0;
    return nDotX / (nDotX * (1.0 - k) + k);
}

// Smith geometry over the view and light directions, image-based k.
float BrdfGeometrySmithIbl(float nDotV, float nDotL, float roughness)
{
    return BrdfGeometrySchlickGGXIbl(nDotV, roughness) *
           BrdfGeometrySchlickGGXIbl(nDotL, roughness);
}

// Schlick's Fresnel approximation for a base reflectance f0.
float3 BrdfFresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// Roughness-aware Fresnel for the image-based ambient term: a rough surface's grazing reflectance
// is bounded by its f0 rather than rising to white.
float3 BrdfFresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    float3 fr = max(float3(1.0 - roughness), f0);
    return f0 + (fr - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// The Van der Corput radical inverse in base 2: the bits of i mirrored about the binary point.
float BrdfRadicalInverse(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

// The i-th of n Hammersley points in [0,1)², a low-discrepancy set for quasi-Monte Carlo
// integration.
float2 BrdfHammersley(uint i, uint n)
{
    return float2(float(i) / float(n), BrdfRadicalInverse(i));
}

// A GGX-distributed half vector about the unit normal for a GGX width alpha, from a point xi in
// [0,1)². Its density is BrdfDistributionGGX · N·H.
float3 BrdfImportanceSampleGGX(float2 xi, float3 normal, float alpha)
{
    float phi = 2.0 * BrdfPi * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    float3 h = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(tangent * h.x + bitangent * h.y + normal * h.z);
}
