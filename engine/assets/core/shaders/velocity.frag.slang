// TAA velocity prepass fragment stage. Writes the per-object screen-space motion
// vector into the RG velocity target: the UV displacement from this pixel's previous
// position to its current one, so the TAA resolve fetches history at uv - velocity.
//
// The motion combines camera and object movement automatically — a static object
// under a moving camera reprojects with the camera (its previous world equals its
// current), and a moving object adds its own displacement on top. Only geometry
// writes here; the background keeps the cleared zero and the resolve falls back to a
// depth-based camera reprojection there.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float4 v_CurClip : TEXCOORD0;
    float4 v_PrevClip : TEXCOORD1;
};

[shader("fragment")]
float2 fsMain(VSOutput input) : SV_Target0
{
    const float2 curUv = (input.v_CurClip.xy / input.v_CurClip.w) * 0.5 + 0.5;
    const float2 prevUv = (input.v_PrevClip.xy / input.v_PrevClip.w) * 0.5 + 0.5;
    return curUv - prevUv;
}
