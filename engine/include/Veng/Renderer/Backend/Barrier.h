#pragma once

// Internal image-barrier helpers. The public API does not expose barriers or
// layouts — transitions are either derived by the render graph from declared
// passes (RenderGraph) or emitted here by the engine's own transfer/present
// paths (Image upload/download, ImGui, present).
#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Types.h>

#include <vector>

namespace Veng::Renderer
{
    class CommandBuffer;
    class Image;
    class Buffer;
}

namespace Veng::Renderer::Backend
{
    /// @brief Layout-only transition: derives destination stage/access from the
    /// target layout (used by out-of-graph paths: upload, download, present).
    ///
    /// Reads the source state from the image's tracked per-subresource state and
    /// updates it to reflect the transition.
    void TransitionImage(CommandBuffer& cmd, Image& image, ImageLayout newLayout, u32 baseLayer = 0,
                         u32 layerCount = 1, u32 baseMip = 0, u32 mipCount = 1);

    /// @brief Explicit transition used by the render graph: the caller supplies
    /// the destination layout/stage/access for a declared use.
    ///
    /// The source side comes from the image's tracked state. No barrier is
    /// emitted for a read-after-read that needs none — the tracked read scope is
    /// widened instead.
    void TransitionImage(CommandBuffer& cmd, Image& image, vk::ImageLayout newLayout,
                         vk::PipelineStageFlags dstStage, vk::AccessFlags dstAccess, u32 baseLayer,
                         u32 layerCount, u32 baseMip, u32 mipCount);

    /// @brief Collects a run of image and buffer barriers and records them as one
    /// vkCmdPipelineBarrier.
    ///
    /// The render graph replays a pass's transitions through one batch, so a pass costs one
    /// barrier command however many resources it transitions. Each add decides its barrier
    /// against the image's tracked state and updates that state immediately, exactly as
    /// TransitionImage does, so a later add sees the earlier one. The recorded command's stage
    /// masks are the union of every barrier's, which orders each barrier at least as strongly as
    /// its own scopes would. An add whose subresource range overlaps a barrier still pending on the
    /// same image records the pending batch first, so two transitions of one subresource keep their
    /// order.
    class BarrierBatch
    {
    public:
        /// @brief Adds the transition of an image subresource range to a declared use.
        ///
        /// Same decision and tracked-state update as the explicit TransitionImage overload; a
        /// read-after-read that needs no barrier adds nothing.
        /// @param cmd        Command buffer a forced early flush is recorded into.
        /// @param image      The image to transition.
        /// @param newLayout  The destination layout.
        /// @param dstStage   The destination stage(s) of the declared use.
        /// @param dstAccess  The destination access of the declared use.
        /// @param baseLayer  First array layer of the range.
        /// @param layerCount Number of array layers in the range.
        /// @param baseMip    First mip level of the range.
        /// @param mipCount   Number of mip levels in the range.
        void AddImage(CommandBuffer& cmd, Image& image, vk::ImageLayout newLayout,
                      vk::PipelineStageFlags dstStage, vk::AccessFlags dstAccess, u32 baseLayer,
                      u32 layerCount, u32 baseMip, u32 mipCount);

        /// @brief Adds a whole-buffer memory barrier between two stage/access scopes.
        /// @param buffer    The buffer to barrier.
        /// @param srcStage  Pipeline stage(s) of the producing access.
        /// @param srcAccess Access flags of the producing access.
        /// @param dstStage  Pipeline stage(s) of the consuming access.
        /// @param dstAccess Access flags of the consuming access.
        void AddBuffer(Buffer& buffer, vk::PipelineStageFlags srcStage, vk::AccessFlags srcAccess,
                       vk::PipelineStageFlags dstStage, vk::AccessFlags dstAccess);

        /// @brief Records every pending barrier as one command and empties the batch.
        ///
        /// Records nothing when no add needed a barrier.
        /// @param cmd Command buffer the barrier is recorded into.
        void Record(CommandBuffer& cmd);

    private:
        /// @brief Pending image barriers, in add order.
        std::vector<vk::ImageMemoryBarrier> m_Images;
        /// @brief Pending buffer barriers, in add order.
        std::vector<vk::BufferMemoryBarrier> m_Buffers;
        /// @brief Union of the pending barriers' source stages.
        vk::PipelineStageFlags m_SrcStages;
        /// @brief Union of the pending barriers' destination stages.
        vk::PipelineStageFlags m_DstStages;
    };

    /// @brief Marks every subresource of an image as produced on @p producingFamily,
    /// carrying the transfer-timeline value its copy signalled.
    ///
    /// The first graphics use reads this to emit the ownership-acquire half and
    /// fold the timeline value into the frame submit. Called by the async upload
    /// worker after recording its copy.
    void MarkProducedOn(Image& image, u32 producingFamily, u64 transferValue);

    /// @brief Emits a buffer-memory barrier over the whole buffer between two
    /// stage/access scopes.
    ///
    /// A buffer has no layout, so the barrier is purely a stage/access dependency
    /// (no transition). The render graph derives the source and destination scopes
    /// from a producing pass's declared write and a consuming pass's declared read
    /// (e.g. a compute StorageBufferWrite followed by an IndirectRead). Buffers do
    /// not carry per-resource tracked state, so both scopes are supplied by the caller.
    /// @param cmd       Command buffer the barrier is recorded into.
    /// @param buffer    The buffer to barrier.
    /// @param srcStage  Pipeline stage(s) of the producing access.
    /// @param srcAccess Access flags of the producing access.
    /// @param dstStage  Pipeline stage(s) of the consuming access.
    /// @param dstAccess Access flags of the consuming access.
    void TransitionBuffer(CommandBuffer& cmd, Buffer& buffer, vk::PipelineStageFlags srcStage,
                          vk::AccessFlags srcAccess, vk::PipelineStageFlags dstStage,
                          vk::AccessFlags dstAccess);

    /// @brief Records the release half of a transfer→graphics queue-family ownership
    /// transfer for the whole image (TransferDst → ShaderReadOnly).
    ///
    /// A no-op when the families match (single-queue collapse). Recorded on the
    /// transfer queue's command buffer; its acquire counterpart is emitted on first
    /// graphics use.
    void ReleaseImageToGraphicsQueue(CommandBuffer& cmd, Image& image, u32 transferFamily,
                                     u32 graphicsFamily);
}
