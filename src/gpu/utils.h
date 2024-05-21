#ifndef _GPU_UTILS_H_
#define _GPU_UTILS_H_

#include "../gfxcommon.h"

VkViewport vkViewport(float x, float y, float w, float h, float minDepth, float maxDepth);
VkRect2D vkRect2D(int32_t x, int32_t y, uint32_t w, uint32_t h);

#endif