// Set 4: the image-based-lighting maps (off the bindless registry, a closed renderer-owned set).
// Irradiance is the diffuse term; the prefiltered cube + the BRDF LUT are the split-sum specular
// term. Binding 0 (the radiance cube) is the skybox pass's and is unused here; it is declared so the
// following fields keep their bindings. Always bound and
// valid; sampling is gated on the view block's ambient arm. Shared by the lighting core
// (Veng/lighting.slang) and by Veng/medium_lighting.slang, which declares the set so a forward-lit
// translucent material's sets stay contiguous.

#pragma once

#include "binding.slang"

struct IblSet
{
    TextureCube Radiance;
    TextureCube Irradiance;
    TextureCube Prefilter;
    Texture2D BrdfLut;
    SamplerState Sampler;
};
VE_DESCRIPTOR_SET(4, IblSet, g_Ibl);

#define g_Irradiance g_Ibl.Irradiance
#define g_Prefilter g_Ibl.Prefilter
#define g_BrdfLut g_Ibl.BrdfLut
#define g_IblSampler g_Ibl.Sampler
