// Linearly Transformed Cosines (Heitz et al.): the geometry-agnostic core of the area-light
// integral, shared by the deferred lighting pass and a test shader so both evaluate the identical
// math. An area light's response is the analytic integral of a clamped-cosine distribution warped
// by a per-(roughness, view-angle) matrix; the polygon integral sums per-edge vector terms
// (LtcIntegrateEdgeVec) into the polygon's vector form factor, then resolves the horizon through
// LtcClippedFormFactor. The pass supplies the LUT matrix, the g-buffer bindings, and the Rect/
// Polygon vertex integrator (LtcPolygon); those name bindings and stay with it.
//
// The form factor these return is the normalized clamped-cosine fraction in [0, 1] — a facing disk
// of angular rim-radius a returns sin^2(a) — so the caller multiplies it by the emitter radiance and
// the BRDF terms directly, with no further solid-angle normalization.

static const float LtcPi = 3.14159265358979323846;
// A Sphere light is integrated as a shading-point-facing disk of this many edges.
static const uint LtcSphereSegments = 16;

float3 LtcIntegrateEdgeVec(float3 v1, float3 v2)
{
    float x = dot(v1, v2);
    float y = abs(x);
    float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
    float b = 3.4175940 + (4.1616724 + y) * y;
    float v = a / b;
    float theta_sintheta = (x > 0.0) ? v : 0.5 * rsqrt(max(1.0 - x * x, 1e-7)) - v;
    return cross(v1, v2) * theta_sintheta;
}

// Horizon-clipped form factor of a spherical cap whose unclipped vector form factor has length len
// and axis cosine z (the analytic sphere irradiance with horizon of Snyder, as formulated in
// Lagarde & de Rousiers, "Moving Frostbite to PBR"). The integrators treat the transformed polygon
// as the proportional cap — exact for the facing disk, and the standard clipless approximation for
// a warped Rect/Polygon. The raw edge sum is exact only for a polygon fully above the transformed
// horizon; a straddling polygon's below-horizon part subtracts, and clamping that sum to zero would
// cut to zero while the true integral is still positive, drawing a hard boundary across a lit
// surface.
float LtcClippedFormFactor(float z, float len)
{
    float sinSigmaSqr = min(len, 0.9999);
    float cosTheta = clamp(z, -0.9999, 0.9999);
    float e;
    if (cosTheta * cosTheta > sinSigmaSqr)
    {
        // Cap fully above (or below) the horizon: the unclipped product survives the clamp.
        e = LtcPi * sinSigmaSqr * max(cosTheta, 0.0);
    }
    else
    {
        float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
        float x = sqrt(1.0 / sinSigmaSqr - 1.0);
        float y = -x * (cosTheta / sinTheta);
        float sinThetaSqrtY = sinTheta * sqrt(1.0 - y * y);
        e = (cosTheta * acos(y) - x * sinThetaSqrtY) * sinSigmaSqr + atan(sinThetaSqrtY / x);
    }
    return max(e, 0.0) / LtcPi;
}

// The LTC integral of a Sphere light, approximated as a shading-point-facing disk of
// LtcSphereSegments edges. The disk is the sphere's silhouette (tangent) circle — radius r*cos(t)
// at distance d*cos^2(t) from the shading point, sin(t) = r/d — so it subtends the sphere's true
// solid angle. A disk built at the sphere centre with the sphere's own radius subtends only
// atan(r/d), not asin(r/d), and under-integrates by up to 2x as the source grows to fill the sky.
// Minv is the fitted matrix (specular) or identity (Lambert diffuse).
float LtcSphere(float3 N, float3 V, float3 P, float3x3 Minv, float3 center, float radius)
{
    float3 T1 = normalize(V - N * dot(V, N));
    float3 T2 = cross(N, T1);
    float3x3 M = mul(Minv, float3x3(T1, T2, N));

    float3 toCenter = center - P;
    float dist = length(toCenter);
    float3 dir = dist > 1e-4 ? toCenter / dist : float3(0.0, 0.0, 1.0);

    float sinTheta = clamp(radius / max(dist, 1e-4), 0.0, 0.9999);
    float cosTheta = sqrt(1.0 - sinTheta * sinTheta);
    float3 diskCenter = P + dir * (dist * cosTheta * cosTheta);
    float diskRadius = radius * cosTheta;

    float3 up = abs(dir.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 tt = normalize(cross(up, dir));
    // Wound so the disk's front face points back toward the shading point.
    float3 bb = cross(tt, dir);

    float3 prev = normalize(mul(M, (diskCenter + diskRadius * tt) - P));
    float3 vsum = float3(0.0);
    for (uint i = 1; i <= LtcSphereSegments; ++i)
    {
        float ang = (2.0 * LtcPi) * float(i) / float(LtcSphereSegments);
        float3 vert = diskCenter + diskRadius * (cos(ang) * tt + sin(ang) * bb);
        float3 cur = normalize(mul(M, vert - P));
        vsum += LtcIntegrateEdgeVec(prev, cur);
        prev = cur;
    }

    float len = length(vsum);
    if (len < 1e-6)
    {
        return 0.0;
    }
    // The facing disk always fronts the shading point, so there is no behind case; the front-lit
    // axis cosine is the negated z (see LtcPolygon).
    return LtcClippedFormFactor(-vsum.z / len, len);
}
