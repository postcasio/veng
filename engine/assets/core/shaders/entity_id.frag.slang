// Entity-id picking fragment stage. Pairs with entity_id.vert / entity_id_skinned.vert: it reads
// the per-draw entity slot index those stages forwarded (v_EntityIndex at location 0) and writes
// the pick id — the index + 1 — into the single R32Uint EntityId target. 0 is reserved as the
// cleared no-hit sentinel, so a live entity is always >= 1 and the readback resolves
// index = pickId - 1.
//
// The picking pipeline is depth-tested against the same geometry, so the nearest visible surface
// wins each texel. It is built and run only when SceneRendererSettings::Picking is set, so the
// shipping g-buffer path never binds this shader.
struct VSOutput
{
    float4 sv_position : SV_Position;
    nointerpolation uint v_EntityIndex : TEXCOORD0;
};

[shader("fragment")]
uint fsMain(VSOutput input) : SV_Target0
{
    return input.v_EntityIndex + 1u;
}
