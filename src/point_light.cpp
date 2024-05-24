#include "point_light.h"
#include "object.h"
#include "engine.h"

PointLight::PointLight()
    : Object(nullptr)
{
  type = ObjectType::PointLight;

  color = glm::vec3(1.0f, 1.0f, 1.0f);
  intensity = 1.0f;
  constant = 1.0f;
  linear = 0.007f;
  quadratic = 0.0002f;
  radius = 10.0f;

  VkFormat depthFormat = renderer()->physicalDevice->findDepthFormat();

  shadowMaps.resize(renderer()->swapChain->imageCount);

  shadowMapViews.resize(renderer()->swapChain->imageCount);

  for (int32_t i = 0; i < renderer()->swapChain->imageCount; i++)
  {
    shadowMaps[i] = std::make_unique<ImageAllocation>(
        SHADOW_MAP_RESOLUTION, SHADOW_MAP_RESOLUTION,
        renderer()->msaaSamples,
        ImageType::TextureCube,
        depthFormat,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        "Shadow Map");
    shadowMaps[i]->transitionImageLayout(
        depthFormat, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    shadowMapViews[i] = shadowMaps[i]->createView(depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
  }

  sampler = device()->createSampler();
  shadowMapFramebuffer = std::make_unique<Framebuffer>(renderer()->swapChain.get(), renderer()->shadowRenderPass.get(), shadowMapViews);
  shadowUniforms = std::make_unique<ShadowUniforms>();
  shadowDescriptorSet = renderer()->descriptorPool->createDescriptorSet(*renderer()->shadowMatricesDescriptorSetLayout);

  updateShadowMatricesDescriptorSets();
}

PointLight::~PointLight()
{
  std::cout << "PointLight::~PointLight()" << std::endl;
  shadowMaps.clear();
  shadowMapViews.clear();
  sampler.reset();
  shadowMapFramebuffer.reset();
  shadowUniforms.reset();
}

void PointLight::updateShadowMatricesDescriptorSets()
{
  for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
  {
    VkDescriptorBufferInfo bufferInfoShadow{};
    bufferInfoShadow.buffer =
        shadowUniforms->uniformBufferAllocations[i]->buffer;
    bufferInfoShadow.offset = 0;
    bufferInfoShadow.range = sizeof(ShadowUniformBufferObject);

    std::array<VkWriteDescriptorSet, 1> descriptorWrites{};

    descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[0].dstSet = shadowDescriptorSet->sets[i];
    descriptorWrites[0].dstBinding = 0;
    descriptorWrites[0].dstArrayElement = 0;
    descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pBufferInfo = &bufferInfoShadow;

    vkUpdateDescriptorSets(device()->device,
                           static_cast<uint32_t>(descriptorWrites.size()),
                           descriptorWrites.data(), 0, nullptr);
  }
}