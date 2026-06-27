// Entity-id picking billboard fragment stage. Pairs with entity_id_billboard.vert: it masks the
// proxy quad to a centered disc and writes the forwarded pick id into the single R32Uint EntityId
// target. The disc keeps the icon's hit target predictable and art-independent (the fixed min-size
// proxy footprint, not the icon's alpha). Occlusion is the hardware depth test against the scene's
// picking depth — an occluded fragment is discarded, not faded, so an icon behind geometry is not
// picked. Built and run only when SceneRendererSettings::Picking is set.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_Quad : TEXCOORD0;
    nointerpolation uint v_PickId : TEXCOORD1;
};

[shader("fragment")]
uint fsMain(VSOutput input) : SV_Target0
{
    // Mask the [-1, 1] proxy quad to a centered unit disc: corners outside it write nothing.
    if (dot(input.v_Quad, input.v_Quad) > 1.0)
    {
        discard;
    }
    return input.v_PickId;
}
