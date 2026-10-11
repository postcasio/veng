// The stochastic reflection chain's ray direction: a microfacet normal drawn from the GGX
// distribution of visible normals (Heitz, "Sampling the GGX Distribution of Visible Normals", JCGT
// 7(4), 2018; isotropic), with the view ray reflected about it, from a 2D sample the per-pixel
// blue-noise tile gives and the view's frame count rotates.
//
// α is GGX α throughout: the squared perceptual roughness. The lobe is not truncated and no pdf is
// returned, so a grazing view reflects part of its lobe below the surface.
//
// SampleGgxVndf and SampleReflectionVector follow the AMD FidelityFX SDK's stochastic screen-space
// reflections, under this licence:
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
// OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef VENG_SSR_SAMPLE_SLANG
#define VENG_SSR_SAMPLE_SLANG

static const float SsrSamplePi = 3.14159265358979;

// Below this α a surface is a mirror: it reflects the view ray exactly, and the denoiser leaves it
// unfiltered.
static const float SsrMirrorAlpha = 1e-4;

// The blue-noise tile's edge in pixels; it repeats across the screen.
static const uint SsrBlueNoiseSize = 128u;

// The golden-ratio rotation each frame adds to the tile, wrapping every 256 frames.
static const float SsrBlueNoiseRotation = 0.61803398875;

// A microfacet normal drawn from the GGX distribution of normals visible from `Ve`, both in the
// frame whose +Z is the surface normal, for uniform `U1`, `U2` in [0, 1). The normal's density is
// G1(Ve)·max(0, Ve·N)·D(N) / Ve.z.
float3 SampleGgxVndf(float3 Ve, float alphaX, float alphaY, float U1, float U2)
{
    // Section 3.2: the view direction in the hemisphere configuration.
    float3 Vh = normalize(float3(alphaX * Ve.x, alphaY * Ve.y, Ve.z));
    // Section 4.1: an orthonormal basis, with the special case of a zero cross product.
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 0.0 ? float3(-Vh.y, Vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
    float3 T2 = cross(Vh, T1);
    // Section 4.2: the projected area's parameterisation.
    float r = sqrt(U1);
    float phi = 2.0 * SsrSamplePi * U2;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2;
    // Section 4.3: reprojection onto the hemisphere.
    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    // Section 3.4: back to the ellipsoid configuration.
    return normalize(float3(alphaX * Nh.x, alphaY * Nh.y, max(0.0, Nh.z)));
}

// The direction a ray leaving a surface along `viewRay` (unit, from the eye toward the surface)
// reflects in, about a microfacet normal drawn from the isotropic GGX lobe of `alpha` around the
// unit `normal`, for the 2D sample `u`. Every vector is in one frame. A mirror (alpha below
// SsrMirrorAlpha) reflects about the normal itself.
float3 SampleReflectionVector(float3 viewRay, float3 normal, float alpha, float2 u)
{
    if (alpha < SsrMirrorAlpha)
        return reflect(viewRay, normal);

    float3 U;
    float3 N = normal;
    if (abs(N.z) > 0.0)
    {
        float k = sqrt(N.y * N.y + N.z * N.z);
        U = float3(0.0, -N.z / k, N.y / k);
    }
    else
    {
        float k = sqrt(N.x * N.x + N.y * N.y);
        U = float3(N.y / k, -N.x / k, 0.0);
    }

    // The tangent frame's rows: tangent, bitangent, normal.
    float3 row0 = U;
    float3 row1 = cross(N, U);
    float3 row2 = N;

    float3 toEyeTbn = float3(dot(row0, -viewRay), dot(row1, -viewRay), dot(row2, -viewRay));
    float3 microNormalTbn = SampleGgxVndf(toEyeTbn, alpha, alpha, u.x, u.y);
    float3 reflectedTbn = reflect(-toEyeTbn, microNormalTbn);

    // Back out of the tangent frame: the transpose of its rows.
    return reflectedTbn.x * row0 + reflectedTbn.y * row1 + reflectedTbn.z * row2;
}

// The 2D sample a pixel draws this frame from the blue-noise tile (two independent void-and-cluster
// channels, RG8): the tile's value at the pixel, at its 256-level stratum's centre, rotated by the
// golden ratio once per frame. `frameIndex` is the view's own frame count, so the rotation has no
// period in common with a projection jitter's.
float2 SsrBlueNoise(Texture2D tile, uint2 pixel, uint frameIndex)
{
    float2 stored = tile.Load(int3(pixel % SsrBlueNoiseSize, 0)).rg;
    float2 stratum = (floor(stored * 255.0 + 0.5) + 0.5) / 256.0;
    return frac(stratum + float(frameIndex & 0xFFu) * SsrBlueNoiseRotation);
}

#endif
