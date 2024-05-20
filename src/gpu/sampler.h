#ifndef _GPU_SAMPLER_H_
#define _GPU_SAMPLER_H_

#include "../gfxcommon.h"

class Sampler
{
public:
    Sampler();
    ~Sampler();

    VkSampler sampler;
    VkSamplerCreateInfo samplerCreateInfo;
};

#endif