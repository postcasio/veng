#pragma once

// The render graph's hook into the backend's barrier recording. The graph decides every transition
// device-free (RenderGraphSchedule.h, BarrierDecision.h) and hands each to a BarrierBatch; the
// backend compiled in implements the batch, deciding each image barrier against the image's live
// tracked state and recording the pending set as one barrier command.

#include "BarrierDecision.h"

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    class Buffer;
    class CommandBuffer;
    class Image;

    /// @brief Collects a run of image and buffer barriers and records them as one barrier command.
    ///
    /// The render graph replays a pass's transitions through one batch, so a pass costs one
    /// barrier command however many resources it transitions. Each add decides its barrier
    /// against the image's tracked state and updates that state immediately, so a later add sees
    /// the earlier one. The recorded command's stage masks are the union of every barrier's,
    /// which orders each barrier at least as strongly as its own scopes would. An add whose
    /// subresource range overlaps a barrier still pending on the same image records the pending
    /// batch first, so two transitions of one subresource keep their order.
    class VE_API BarrierBatch
    {
    public:
        /// @brief Adds the transition of an image subresource range to a declared use.
        ///
        /// A read-after-read inside the tracked read scope adds nothing (DecideBarrier).
        /// @param cmd        Command buffer a forced early flush is recorded into.
        /// @param image      The image to transition.
        /// @param newLayout  The destination layout.
        /// @param dstStage   The destination stage(s) of the declared use.
        /// @param dstAccess  The destination access of the declared use.
        /// @param baseLayer  First array layer of the range.
        /// @param layerCount Number of array layers in the range.
        /// @param baseMip    First mip level of the range.
        /// @param mipCount   Number of mip levels in the range.
        void AddImage(CommandBuffer& cmd, Image& image, ImageLayout newLayout, SyncStage dstStage,
                      SyncAccess dstAccess, u32 baseLayer, u32 layerCount, u32 baseMip,
                      u32 mipCount);

        /// @brief Adds a whole-buffer memory barrier between two stage/access scopes.
        /// @param buffer    The buffer to barrier.
        /// @param srcStage  Pipeline stage(s) of the producing access.
        /// @param srcAccess Access of the producing access.
        /// @param dstStage  Pipeline stage(s) of the consuming access.
        /// @param dstAccess Access of the consuming access.
        void AddBuffer(Buffer& buffer, SyncStage srcStage, SyncAccess srcAccess, SyncStage dstStage,
                       SyncAccess dstAccess);

        /// @brief Records every pending barrier as one command and empties the batch.
        ///
        /// Records nothing when no add needed a barrier.
        /// @param cmd Command buffer the barrier is recorded into.
        void Record(CommandBuffer& cmd);

    private:
        /// @brief One pending image barrier: the image, the decided scopes and the range.
        struct PendingImage
        {
            /// @brief The image the barrier transitions.
            Image* Target = nullptr;
            /// @brief The barrier's source scope.
            SubresourceState Src;
            /// @brief The barrier's destination scope.
            SubresourceState Dst;
            /// @brief Source queue family of an ownership transfer, or QueueFamilyIgnored.
            u32 SrcQueueFamily = QueueFamilyIgnored;
            /// @brief Destination queue family of an ownership transfer, or QueueFamilyIgnored.
            u32 DstQueueFamily = QueueFamilyIgnored;
            /// @brief First array layer of the range.
            u32 BaseLayer = 0;
            /// @brief Number of array layers in the range.
            u32 LayerCount = 1;
            /// @brief First mip level of the range.
            u32 BaseMip = 0;
            /// @brief Number of mip levels in the range.
            u32 MipCount = 1;
        };

        /// @brief One pending whole-buffer barrier.
        struct PendingBuffer
        {
            /// @brief The buffer the barrier orders.
            Buffer* Target = nullptr;
            /// @brief The producing access's stages.
            SyncStage SrcStage = SyncStage::None;
            /// @brief The producing access.
            SyncAccess SrcAccess = SyncAccess::None;
            /// @brief The consuming access's stages.
            SyncStage DstStage = SyncStage::None;
            /// @brief The consuming access.
            SyncAccess DstAccess = SyncAccess::None;
        };

        /// @brief Pending image barriers, in add order.
        vector<PendingImage> m_Images;
        /// @brief Pending buffer barriers, in add order.
        vector<PendingBuffer> m_Buffers;
        /// @brief Union of the pending barriers' source stages.
        SyncStage m_SrcStages = SyncStage::None;
        /// @brief Union of the pending barriers' destination stages.
        SyncStage m_DstStages = SyncStage::None;
    };
}
