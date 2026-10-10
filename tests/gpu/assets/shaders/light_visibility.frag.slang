#include "Veng/translucent.slang"
#include "Veng/medium_lighting.slang"

// Light-visibility probe (gpu fixture): a Translucent fragment that asks the medium lighting
// queries about fixed world points rather than its own surface, and returns the answer at full
// coverage. The screen is cut into four columns and three rows of cells, one probe per cell, and
// every probe reads the light found by tag Select.z through filter Select.y.
//
// Select.x picks the mode:
//   0 — cascade probes. Column c probes the middle of cascade c's depth range on the camera's
//       axis: row 0 offset by Shadowed.xyz, row 1 by Lit.xyz. Row 2 probes the axis at depth
//       Past.w, offset by Past.xyz. Blue is 1 where the probe's depth selects cascade c.
//   1 — fixed probes, the same in every column: row 0 the world point Shadowed.xyz, row 1 Lit.xyz,
//       row 2 Past.xyz.
//   2 — tag lookup: red where FindLightByTag(Select.z) returns Select.w, green where a tag no light
//       carries (Select.z + 1) and the untagged default both return LightIndexInvalid.
struct MaterialParams
{
    float4 Select;
    float4 Shadowed;
    float4 Lit;
    float4 Past;
};

// The camera's optical axis, through the same inverse view-projection the view block carries.
float3 CameraAxis(ViewConstants vc)
{
    float4 centre = mul(vc.InvViewProj, float4(0.0, 0.0, 0.5, 1.0));
    return normalize(centre.xyz / centre.w - vc.CameraPosition.xyz);
}

[shader("fragment")]
float4 fsMain(SurfaceFragmentInput input) : SV_Target0
{
    MaterialParams p = g_MaterialParams.Load<MaterialParams>(input.v_MaterialOffset);
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    uint tag = uint(p.Select.z);

    if (p.Select.x > 1.5)
    {
        uint found = FindLightByTag(vc, tag);
        float matches = found == uint(p.Select.w) ? 1.0 : 0.0;
        float absent = (FindLightByTag(vc, tag + 1u) == LightIndexInvalid &&
                        FindLightByTag(vc, 0u) == LightIndexInvalid)
            ? 1.0
            : 0.0;
        return float4(matches, absent, 0.0, 1.0);
    }

    float2 uv = SceneScreenUV(vc, input.sv_position);
    uint column = min(uint(uv.x * 4.0), 3u);
    uint row = min(uint(uv.y * 3.0), 2u);
    uint filter = p.Select.y > 0.5 ? LightVisibilityPcf : LightVisibilitySingleTap;
    uint light = FindLightByTag(vc, tag);

    float3 probe;
    float cascadeMatches = 0.0;
    if (p.Select.x > 0.5)
    {
        probe = row == 0u ? p.Shadowed.xyz : (row == 1u ? p.Lit.xyz : p.Past.xyz);
    }
    else
    {
        uint set = LightCascadeSet(LoadLight(vc, light).Cone.w);
        float far = g_Shadow.Sets[set].Splits[column];
        float near = column == 0u ? 0.5 * far : g_Shadow.Sets[set].Splits[column - 1u];
        float depth = row == 2u ? p.Past.w : 0.5 * (near + far);
        float3 offset = row == 0u ? p.Shadowed.xyz : (row == 1u ? p.Lit.xyz : p.Past.xyz);
        probe = vc.CameraPosition.xyz + CameraAxis(vc) * depth + offset;
        uint count = uint(g_Shadow.ShadowParams.z);
        float viewDepth = -mul(vc.View, float4(probe, 1.0)).z;
        cascadeMatches = SelectCascade(viewDepth, count, set) == column ? 1.0 : 0.0;
    }

    float visibility = LightVisibility(vc, light, probe, filter);
    return float4(visibility, visibility, cascadeMatches, 1.0);
}
