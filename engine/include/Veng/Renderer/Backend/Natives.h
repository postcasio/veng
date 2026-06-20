#pragma once

// Out-of-line definitions of the Native structs declared (but not defined) by
// the public Renderer headers. Internal-only: backend .cpp files include this
// to reach the raw vk::/Vma handles behind Resource::GetNative().
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Backend/ContextNative.h>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Fence.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Semaphore.h>
#include <Veng/Renderer/ShaderModule.h>
#include <Veng/Renderer/TimelineSemaphore.h>
#include <Veng/Window.h>

namespace Veng
{
    struct Window::Native
    {
        vk::SurfaceKHR Surface;
    };

    struct ImGuiTexture::Native
    {
        vk::DescriptorSet Set;
    };
}

namespace Veng::Renderer
{
    struct Buffer::Native
    {
        vk::Buffer Buffer;
        VmaAllocation Allocation{};
        /// @brief Persistent mapping for a HostMapped buffer (VMA_ALLOCATION_CREATE_MAPPED_BIT);
        /// null for device-local buffers.
        void* MappedData = nullptr;
    };

    struct Image::Native
    {
        vk::Image Image;
        VmaAllocationInfo AllocationInfo{};
        VmaAllocation Allocation = nullptr;

        /// @brief Per-subresource (layer × mip) layout/stage/access tracking —
        /// the state the render graph diffs against to derive barriers.
        ///
        /// Seeded to Undefined/TopOfPipe so the first use of any subresource
        /// emits a correct undefined-source transition.
        struct SubresourceState
        {
            vk::ImageLayout Layout = vk::ImageLayout::eUndefined;
            vk::PipelineStageFlags Stage = vk::PipelineStageFlagBits::eTopOfPipe;
            vk::AccessFlags Access{};

            /// @brief The queue family that last produced this subresource
            /// (see Backend::SubresourceState::ProducingFamily).
            ///
            /// VK_QUEUE_FAMILY_IGNORED means graphics-produced — the default
            /// until an async upload marks it otherwise.
            u32 ProducingFamily = VK_QUEUE_FAMILY_IGNORED;

            /// @brief Transfer-timeline value an async upload signalled for this
            /// subresource.
            ///
            /// The first graphics use folds this into the frame submit as a
            /// transfer wait, then clears it to 0. Zero means no pending wait.
            u64 PendingTransferValue = 0;
        };

        u32 Layers = 1;
        u32 MipLevels = 1;
        vector<SubresourceState> States;

        void InitStates(const u32 layers, const u32 mipLevels)
        {
            Layers = layers;
            MipLevels = mipLevels;
            States.assign(static_cast<usize>(layers) * mipLevels, {});
        }

        [[nodiscard]] SubresourceState& At(const u32 layer, const u32 mip)
        {
            return States[layer * MipLevels + mip];
        }
    };

    struct ImageView::Native
    {
        vk::ImageView ImageView;
    };

    struct Sampler::Native
    {
        vk::Sampler Sampler;
    };

    struct ShaderModule::Native
    {
        vk::ShaderModule Module;
    };

    struct DescriptorSet::Native
    {
        vk::DescriptorSet Set;
    };

    struct DescriptorSetLayout::Native
    {
        vk::DescriptorSetLayout Layout;
    };

    struct PipelineLayout::Native
    {
        vk::PipelineLayout Layout;
    };

    struct GraphicsPipeline::Native
    {
        vk::Pipeline Pipeline;
    };

    struct ComputePipeline::Native
    {
        vk::Pipeline Pipeline;
    };

    struct CommandBuffer::Native
    {
        vk::CommandBuffer CommandBuffer;
        /// @brief The pool this buffer was allocated from.
        ///
        /// A transfer command buffer comes from a per-worker transfer pool, not
        /// the shared graphics pool, and must be freed back to the pool that owns it.
        vk::CommandPool Pool;
    };

    struct Fence::Native
    {
        vk::Fence Fence;
    };

    struct Semaphore::Native
    {
        vk::Semaphore Semaphore;
    };

    struct TimelineSemaphore::Native
    {
        vk::Semaphore Semaphore;
    };
}
