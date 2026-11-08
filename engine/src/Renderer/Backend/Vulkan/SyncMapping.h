#pragma once

// Maps the device-free synchronization vocabulary (BarrierDecision.h) onto Vulkan's pipeline
// stages and access masks. Every engine bit has exactly one Vulkan bit; the image layouts map
// through TypeMapping.h's ToVk(ImageLayout).

#include "Vulkan.h"
#include "../../BarrierDecision.h"

#include <array>
#include <utility>

namespace Veng::Renderer
{
    /// @brief Each engine stage bit beside the Vulkan stage bit it maps to.
    inline constexpr std::array StageBitMap{
        std::pair{SyncStage::TopOfPipe, vk::PipelineStageFlagBits::eTopOfPipe},
        std::pair{SyncStage::DrawIndirect, vk::PipelineStageFlagBits::eDrawIndirect},
        std::pair{SyncStage::VertexShader, vk::PipelineStageFlagBits::eVertexShader},
        std::pair{SyncStage::FragmentShader, vk::PipelineStageFlagBits::eFragmentShader},
        std::pair{SyncStage::EarlyFragmentTests, vk::PipelineStageFlagBits::eEarlyFragmentTests},
        std::pair{SyncStage::LateFragmentTests, vk::PipelineStageFlagBits::eLateFragmentTests},
        std::pair{SyncStage::ColorAttachmentOutput,
                  vk::PipelineStageFlagBits::eColorAttachmentOutput},
        std::pair{SyncStage::ComputeShader, vk::PipelineStageFlagBits::eComputeShader},
        std::pair{SyncStage::Transfer, vk::PipelineStageFlagBits::eTransfer},
        std::pair{SyncStage::BottomOfPipe, vk::PipelineStageFlagBits::eBottomOfPipe},
        std::pair{SyncStage::Host, vk::PipelineStageFlagBits::eHost},
        std::pair{SyncStage::AllCommands, vk::PipelineStageFlagBits::eAllCommands},
    };

    /// @brief Each engine access bit beside the Vulkan access bit it maps to.
    inline constexpr std::array AccessBitMap{
        std::pair{SyncAccess::IndirectCommandRead, vk::AccessFlagBits::eIndirectCommandRead},
        std::pair{SyncAccess::ShaderRead, vk::AccessFlagBits::eShaderRead},
        std::pair{SyncAccess::ShaderWrite, vk::AccessFlagBits::eShaderWrite},
        std::pair{SyncAccess::ColorAttachmentRead, vk::AccessFlagBits::eColorAttachmentRead},
        std::pair{SyncAccess::ColorAttachmentWrite, vk::AccessFlagBits::eColorAttachmentWrite},
        std::pair{SyncAccess::DepthStencilAttachmentRead,
                  vk::AccessFlagBits::eDepthStencilAttachmentRead},
        std::pair{SyncAccess::DepthStencilAttachmentWrite,
                  vk::AccessFlagBits::eDepthStencilAttachmentWrite},
        std::pair{SyncAccess::TransferRead, vk::AccessFlagBits::eTransferRead},
        std::pair{SyncAccess::TransferWrite, vk::AccessFlagBits::eTransferWrite},
        std::pair{SyncAccess::HostRead, vk::AccessFlagBits::eHostRead},
        std::pair{SyncAccess::HostWrite, vk::AccessFlagBits::eHostWrite},
        std::pair{SyncAccess::MemoryRead, vk::AccessFlagBits::eMemoryRead},
        std::pair{SyncAccess::MemoryWrite, vk::AccessFlagBits::eMemoryWrite},
    };

    /// @brief Maps an engine stage mask to the Vulkan pipeline stage mask it names.
    [[nodiscard]] constexpr vk::PipelineStageFlags ToVk(const SyncStage stage)
    {
        vk::PipelineStageFlags flags;
        for (const auto& [engine, vulkan] : StageBitMap)
        {
            if (HasFlag(stage, engine))
            {
                flags |= vulkan;
            }
        }
        return flags;
    }

    /// @brief Maps an engine access mask to the Vulkan access mask it names.
    [[nodiscard]] constexpr vk::AccessFlags ToVk(const SyncAccess access)
    {
        vk::AccessFlags flags;
        for (const auto& [engine, vulkan] : AccessBitMap)
        {
            if (HasFlag(access, engine))
            {
                flags |= vulkan;
            }
        }
        return flags;
    }

    static_assert(QueueFamilyIgnored == VK_QUEUE_FAMILY_IGNORED);
}
