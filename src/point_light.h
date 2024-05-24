#ifndef _POINT_LIGHT_H_
#define _POINT_LIGHT_H_

#include "gfxcommon.h"
#include "object.h"
#include "shadow_uniforms.h"

#define SHADOW_MAP_RESOLUTION 2048
class PointLight : public Object
{
public:
    PointLight();
    ~PointLight();

    glm::vec3 color;
    float intensity;
    float constant;
    float linear;
    float quadratic;
    float radius;

    std::vector<std::unique_ptr<ImageAllocation>> shadowMaps;
    std::vector<std::unique_ptr<ImageView>> shadowMapViews;
    std::unique_ptr<Framebuffer> shadowMapFramebuffer;
    std::unique_ptr<Sampler> sampler;

    std::unique_ptr<ShadowUniforms> shadowUniforms;
    std::unique_ptr<DescriptorSet> shadowDescriptorSet;
    void updateShadowMatricesDescriptorSets();
};

#endif