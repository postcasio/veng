// The binding-free half of the instanced mesh set contract: the instance record's layout, the
// per-instance bound, the projected-size rule level-of-detail selection and the projected-size band
// measure by, and the level selection itself. The cull pass (instanced_mesh_cull.comp) and any
// material (through Veng/instanced_mesh.slang) include it, so a consumer drawing another
// representation of the same instances can test the identical expression the cull tests.
//
// Every function here mirrors a C++ twin (src/Renderer/LodSelection, src/Renderer/InstancedCull)
// that the CPU fallback runs, so the two paths decide alike.

// One instance as stored: an object -> set-frame affine transform as three rows of a 3x4 matrix
// (rotation/scale in xyz, translation in w), and one float4 of consumer data. 64 bytes, matching
// the engine's Renderer::MeshInstance.
struct InstanceRecord
{
    float4 Row0;
    float4 Row1;
    float4 Row2;
    float4 Datum;
};

// The byte size of one InstanceRecord.
static const uint InstanceRecordBytes = 64u;

// The size a sphere containing the eye projects to: unboundedly large.
static const float InstanceSizeUnbounded = 3.402823466e38;

// The instance's object -> set-frame transform as a 4x4 matrix.
float4x4 InstanceMatrix(InstanceRecord record)
{
    return float4x4(record.Row0, record.Row1, record.Row2, float4(0.0, 0.0, 0.0, 1.0));
}

// The largest axis scale of a model matrix's upper 3x3: how far a unit radius reaches.
float MaxAxisScale(float4x4 model)
{
    const float3 c0 = float3(model[0][0], model[1][0], model[2][0]);
    const float3 c1 = float3(model[0][1], model[1][1], model[2][1]);
    const float3 c2 = float3(model[0][2], model[1][2], model[2][2]);
    return max(length(c0), max(length(c1), length(c2)));
}

// The bounding sphere level-of-detail selection measures an object by: its mesh's level-0 bound's
// centre carried through the model matrix, and its half-diagonal times the matrix's largest axis
// scale, so the size does not change as the object turns.
void InstanceBoundingSphere(float4x4 model, float3 boundsMin, float3 boundsMax, out float3 center,
                            out float radius)
{
    center = mul(model, float4((boundsMin + boundsMax) * 0.5, 1.0)).xyz;
    radius = length(boundsMax - boundsMin) * 0.5 * MaxAxisScale(model);
}

// The world-space box enclosing a mesh bound carried through a model matrix.
void InstanceWorldBounds(float4x4 model, float3 boundsMin, float3 boundsMax, out float3 worldMin,
                         out float3 worldMax)
{
    const float3 center = mul(model, float4((boundsMin + boundsMax) * 0.5, 1.0)).xyz;
    const float3 extents = (boundsMax - boundsMin) * 0.5;
    const float3 reach = float3(dot(abs(float3(model[0][0], model[0][1], model[0][2])), extents),
                                dot(abs(float3(model[1][0], model[1][1], model[1][2])), extents),
                                dot(abs(float3(model[2][0], model[2][1], model[2][2])), extents));
    worldMin = center - reach;
    worldMax = center + reach;
}

// The projected size of a sphere: its diameter as a fraction of the view's height, times
// screenScale — radius x projectionScale / distance under a perspective projection, and radius x
// projectionScale under an orthographic one. The distance is to the eye, not the view depth.
// projectionScale is the view-projection's vertical scale (1 / tan(fovY / 2), or 2 / height).
float ProjectedSphereSize(float3 eye, float projectionScale, bool perspective, float screenScale,
                          float3 center, float radius)
{
    if (!perspective)
    {
        return radius * projectionScale * screenScale;
    }
    const float distance = length(center - eye);
    if (distance <= radius)
    {
        return InstanceSizeUnbounded;
    }
    return radius * projectionScale / distance * screenScale;
}

// The vertical scale of a world -> clip matrix: the length of its y row's spatial part.
float ViewProjectionScale(float4x4 viewProj)
{
    return length(float3(viewProj[1][0], viewProj[1][1], viewProj[1][2]));
}

// Whether a world -> clip matrix is a perspective one (its w row has a spatial part).
bool IsPerspectiveViewProjection(float4x4 viewProj)
{
    return length(float3(viewProj[3][0], viewProj[3][1], viewProj[3][2])) > 1e-6;
}

// The size in pixels a sphere projects to in the reference view an instanced set's projected-size
// band is measured against (InstancedMesh::MinProjectedPixels / MaxProjectedPixels): its projected
// size, with no level-of-detail bias, times the view's height in pixels. The renderer passes the
// main camera's unjittered view-projection (ViewConstants.CurViewProj), its position
// (ViewConstants.CameraPosition) and its rendered height (ViewConstants.ExtentParams.y), so a pass
// drawn in that view partitions instances with the cull by evaluating this with those values.
float ReferenceProjectedPixels(float4x4 referenceViewProj, float3 referenceEye,
                               float referenceHeightPixels, float3 center, float radius)
{
    const float size = ProjectedSphereSize(referenceEye, ViewProjectionScale(referenceViewProj),
                                           IsPerspectiveViewProjection(referenceViewProj), 1.0,
                                           center, radius);
    return size >= InstanceSizeUnbounded ? InstanceSizeUnbounded : size * referenceHeightPixels;
}

// The "no level drawn last frame" value of a level memory.
static const uint InstanceNoPreviousLod = 0xFFu;

// Selects a level from a projected size against descending thresholds, with hysteresis: level i
// (i >= 1) is wanted while the size is below thresholds[i - 1], and each threshold is widened into
// a band of +-hysteresis (relative) that a level must cross wholly to change. With no previous
// level the threshold itself decides. thresholds holds count values (at most 7) across two float4s.
uint SelectInstanceLod(float4 thresholdsLow, float4 thresholdsHigh, uint count, float size,
                       uint previous, float hysteresis)
{
    uint coarsest = 0u;
    uint middle = 0u;
    uint finest = 0u;
    for (uint i = 0u; i < count; ++i)
    {
        const float threshold = i < 4u ? thresholdsLow[i] : thresholdsHigh[i - 4u];
        if (size >= threshold * (1.0 + hysteresis))
        {
            break;
        }
        ++finest;
        if (size < threshold)
        {
            ++middle;
        }
        if (size < threshold * (1.0 - hysteresis))
        {
            ++coarsest;
        }
    }
    if (previous == InstanceNoPreviousLod)
    {
        return middle;
    }
    return clamp(previous, coarsest, finest);
}
