// Octahedral direction ↔ UV mapping: the square parameterization of the sphere a SceneCapture's
// output uses, chosen because a 2D map samples through the ordinary bindless Texture2D array (a
// cube view cannot ride set 0). Shared by the engine's capture resample pass and any material
// sampling a capture's output — a consumer computes OctahedralUV(direction) and samples the
// capture texture there. Bindings-free, so it is includable beside any domain contract.
//
// **It is not equal-area.** The fold is `dir / L1norm`, whose solid angle per unit of UV runs as
// `1 / |p|³` over the octahedron point `p`: uniform density would be `4 / edge²` per texel at the
// six vertices and it reaches 5.196 times that at the eight face centres. The distortion is mild
// and has no polar singularity, which is what the mapping is chosen for — but a consumer weighting
// by area, or measuring a distance between two texels, must take the per-texel solid angle and the
// great-circle arc rather than assuming the square's own metric.

// Per-component sign that never returns zero, the octahedral fold's reflection factor.
float2 OctSignNotZero(float2 v)
{
    return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
}

// Maps a unit direction to its [0,1]² octahedral UV.
float2 OctahedralUV(float3 dir)
{
    const float3 a = dir / (abs(dir.x) + abs(dir.y) + abs(dir.z));
    float2 uv = a.z >= 0.0 ? a.xy : (1.0 - abs(float2(a.y, a.x))) * OctSignNotZero(a.xy);
    return uv * 0.5 + 0.5;
}

// Maps a [0,1]² octahedral UV back to its unit direction (OctahedralUV's inverse).
float3 OctahedralDir(float2 uv)
{
    const float2 f = uv * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0)
    {
        const float2 folded = (1.0 - abs(float2(n.y, n.x))) * OctSignNotZero(float2(n.x, n.y));
        n.x = folded.x;
        n.y = folded.y;
    }
    return normalize(n);
}
