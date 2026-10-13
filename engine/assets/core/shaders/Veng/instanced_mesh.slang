// What a Surface material reads of the instance an instanced mesh set drew it for (the
// InstancedMesh component). A material declaring "instancedSets": true in its .vmat.json can be drawn
// by a set; its fragment includes this in place of Veng/surface.slang, which this includes (that file
// has no include guard, so including both defines its declarations twice), and reads the instance
// through the fragment input. A fragment drawn for anything other than a set reads the neutral values
// below, so one material serves a set and an ordinary mesh alike.
//
// The binding-free half — the record layout, the projected-size and level rules — is in
// Veng/instanced_mesh_math.slang, which this includes.
#include "surface.slang"
#include "instanced_mesh_math.slang"

// Whether the fragment was drawn for an instanced set's instance.
bool IsInstanceDraw(SurfaceFragmentInput input)
{
    return input.v_Instance.x != InstanceNone;
}

// The instance's whole stored record, or a zero record for a fragment not drawn for an instance.
InstanceRecord LoadInstance(SurfaceFragmentInput input)
{
    if (!IsInstanceDraw(input))
    {
        InstanceRecord none;
        none.Row0 = float4(1.0, 0.0, 0.0, 0.0);
        none.Row1 = float4(0.0, 1.0, 0.0, 0.0);
        none.Row2 = float4(0.0, 0.0, 1.0, 0.0);
        none.Datum = float4(0.0);
        return none;
    }
    return g_Buffers[input.v_Instance.x].Load<InstanceRecord>(input.v_Instance.y *
                                                              InstanceRecordBytes);
}

// The instance's custom datum (MeshInstance::Datum), or zero for a fragment not drawn for an
// instance.
float4 InstanceDatum(SurfaceFragmentInput input)
{
    return LoadInstance(input).Datum;
}

// The fragment's position in the instance's own object space — the mesh's coordinates before the
// instance's transform — so an effect anchored to the surface stays put however the instance is
// placed, turned or scaled. For an ordinary mesh it is that mesh's object space.
float3 InstanceObjectPosition(SurfaceFragmentInput input)
{
    return input.v_ObjectPosition;
}
