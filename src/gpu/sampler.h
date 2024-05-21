#ifndef _GPU_SAMPLER_H_
#define _GPU_SAMPLER_H_

#include "../gfxcommon.h"
#include "logical_device.h"

class Sampler
{
public:
    Sampler(LogicalDevice &device);
    ~Sampler();

    VkSampler sampler;
    VkSamplerCreateInfo samplerCreateInfo;

    LogicalDevice &device;
};

#endif