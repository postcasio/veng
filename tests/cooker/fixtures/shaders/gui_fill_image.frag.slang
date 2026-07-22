// The second GuiFill fixture fragment: the Image-host material, shading authored art rather than
// replacing it. The widget's own `src` texture and sampler reach it as the two conventional
// runtime-bound handle fields (Gui::ImageMaterialTextureField / ImageMaterialSamplerField), written
// once when the element resolves, and the fill desaturates that texel toward the authored tint.
#include "Veng/guifill.slang"

struct MaterialParams
{
    float4 Tint;
    uint Image;        // bindless sampled-image index of the Image element's src texture
    uint ImageSampler; // bindless sampler index of that texture's sampler
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
float4 fsMain(GuiFillInputs input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);
    float4 texel = g_Textures[NonUniformResourceIndex(p.Image)].Sample(
        g_Samplers[NonUniformResourceIndex(p.ImageSampler)], input.v_UV);
    float luma = dot(texel.rgb, float3(0.299, 0.587, 0.114));
    float4 fill = float4(lerp(texel.rgb, luma * p.Tint.rgb, 0.8), texel.a * p.Tint.a);
    fill.a *= input.v_Color.a;
    return GuiFillResolve(input, fill);
}
