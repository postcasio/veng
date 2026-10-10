// Set 4: the image-based-lighting maps (off the bindless registry, a closed renderer-owned set).
// Irradiance is the diffuse term; the prefiltered cube + the BRDF LUT are the split-sum specular
// term. Binding 0 (the radiance cube) is the skybox pass's and is unused here. Always bound and
// valid; sampling is gated on the view block's ambient arm. Shared by the lighting core
// (Veng/lighting.slang) and by Veng/medium_lighting.slang, which declares the set so a forward-lit
// translucent material's sets stay contiguous.

#pragma once

[[vk::binding(1, 4)]] TextureCube g_Irradiance;
[[vk::binding(2, 4)]] TextureCube g_Prefilter;
[[vk::binding(3, 4)]] Texture2D g_BrdfLut;
[[vk::binding(4, 4)]] SamplerState g_IblSampler;
