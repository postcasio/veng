// The legend swatch's fill: a GuiFill material on the <Image> host, shading the widget's own art
// rather than replacing it. The element's `src` texture and sampler arrive as the two conventional
// runtime-bound handle fields (Gui::ImageMaterialTextureField / ImageMaterialSamplerField), written
// once when the element resolves, so the shader samples exactly what the plain fill would have
// drawn and then recolors it — here a scanline wipe on the pass clock between the texel and a tint.
#include "Veng/guifill.slang"

struct MaterialParams
{
    float4 Tint;       // the color the wipe pushes the texel toward
    uint Image;        // bindless sampled-image index of the element's src texture
    uint ImageSampler; // bindless sampler index of that texture's sampler
    float Speed;       // wipe speed, in passes per second
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
    float2 box = GuiFillBoxCoord(input);
    float wipe = saturate(0.5 + 0.5 * sin(box.y * 4.0 + g_PC.Time * p.Speed));
    float4 fill = float4(lerp(texel.rgb, p.Tint.rgb, wipe), texel.a * p.Tint.a);
    fill.a *= input.v_Color.a;
    return GuiFillResolve(input, fill);
}
