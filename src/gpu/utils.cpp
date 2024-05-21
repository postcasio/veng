#include "utils.h"

VkViewport vkViewport(float x, float y, float w, float h, float minDepth, float maxDepth)
{
    VkViewport viewport{
        .x = x,
        .y = y,
        .width = w,
        .height = h,
        .minDepth = minDepth,
        .maxDepth = maxDepth};

    return viewport;
}

VkRect2D vkRect2D(int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    VkRect2D rect2D{
        .offset = {x, y},
        .extent = {w, h}};

    return rect2D;
}