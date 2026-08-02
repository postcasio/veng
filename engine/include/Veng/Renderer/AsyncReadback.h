#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Buffer;
    class CommandBuffer;
    class Context;
    class Image;
    class ImageView;

    /// @brief Identity of a readback in flight, minted by AsyncReadback::Request.
    struct AsyncReadbackHandle
    {
        /// @brief Sentinel for a readback that was never requested.
        static constexpr u64 Invalid = 0;
        /// @brief Monotonic request id.
        u64 Id = Invalid;
        /// @brief Returns true if the handle names a requested readback.
        [[nodiscard]] bool IsValid() const { return Id != Invalid; }
    };

    /// @brief One image subresource to copy back to the host without blocking the render thread.
    struct AsyncReadbackRequest
    {
        /// @brief Debug name for the staging buffer.
        string Name = "AsyncReadback";
        /// @brief The image to read. Must carry ImageUsage::TransferSrc.
        Ref<Veng::Renderer::Image> Image;
        /// @brief The mip level to read.
        u32 MipLevel = 0;
        /// @brief The array layer to read.
        u32 ArrayLayer = 0;
        /// @brief The access the subresource is left prepared for once the copy is recorded.
        ///
        /// The copy needs TransferSrc, so the subresource is transitioned there and then back to
        /// this. Sample is the default because a generated texture's steady state is being sampled,
        /// and a bindless-sampled image left in TransferSrc would be read in the wrong layout.
        AccessKind RestoreTo = AccessKind::Sample;
        /// @brief Delivered on the main thread once the copy has completed, with the level's bytes.
        ///
        /// The span is tightly packed in the image's format and is valid only for the duration of
        /// the call — copy what is wanted out of it.
        function<void(std::span<const u8>)> OnComplete;
    };

    /// @brief Frame-deferred image readback: the copy rides the frame command buffer and the bytes
    ///        are delivered frames later, with no fence wait on the render thread.
    ///
    /// Owned by Context and pumped once per frame from BeginFrame, so a request placed on any frame
    /// is staged into that frame's command buffer and delivered once GetMaxFramesInFlight() frames
    /// have passed — the point at which the staging frame's fence has provably been waited. There
    /// is no blocking path to call: the class never submits, never waits a fence, and never idles
    /// the device. Image::Download stays the synchronous sibling for tooling that genuinely wants
    /// the bytes now (a screenshot); nothing on a frame path should use it.
    ///
    /// Usable with or without GeneratedTextureService — it reads any image carrying
    /// ImageUsage::TransferSrc.
    class AsyncReadback
    {
    public:
        /// @brief Constructs the readback pump for a context.
        /// @param context The render context staging buffers are created on.
        explicit AsyncReadback(Context& context);

        /// @brief Drops every pending readback and its staging buffer through the retire path.
        ~AsyncReadback();

        AsyncReadback(const AsyncReadback&) = delete;
        AsyncReadback& operator=(const AsyncReadback&) = delete;

        /// @brief Queues a readback; the copy is staged at the next pump and delivered frames later.
        ///
        /// The staging buffer is allocated here, sized from the subresource's format and extent, so
        /// an unsupported (zero-byte) format is rejected at the call rather than silently read as
        /// nothing.
        /// @param request The image subresource to read and the completion to deliver it to.
        /// @return A handle naming the pending readback, or an invalid handle when the request was
        ///         rejected (no image, out-of-range subresource, or a format with no known size).
        AsyncReadbackHandle Request(AsyncReadbackRequest request);

        /// @brief Drops a pending readback; its completion never fires.
        /// @param handle The handle Request returned.
        /// @return True when a pending readback was dropped.
        bool Cancel(AsyncReadbackHandle handle);

        /// @brief The number of readbacks requested but not yet delivered.
        [[nodiscard]] u32 GetPendingCount() const { return static_cast<u32>(m_Pending.size()); }

        /// @brief The number of readbacks delivered over this pump's lifetime.
        [[nodiscard]] u64 GetCompletedCount() const { return m_CompletedCount; }

    private:
        friend class Context;

        /// @brief Delivers every ready readback, then stages every newly requested one.
        ///
        /// Called once per frame by Context::BeginFrame with the frame's command buffer, before any
        /// pass records. Delivery precedes staging so a readback never waits an extra frame.
        /// @param cmd The frame's command buffer the copies are recorded into.
        void Pump(CommandBuffer& cmd);

        /// @brief One readback's staging buffer, target subresource, and progress.
        struct Entry
        {
            /// @brief The handle Request minted for this readback.
            u64 Id = 0;
            /// @brief The image being read; held so it cannot be destroyed under the copy.
            Ref<Veng::Renderer::Image> Image;
            /// @brief A view over exactly the subresource being read, for the layout transitions.
            Ref<ImageView> View;
            /// @brief Host-mapped destination of the copy.
            Ref<Buffer> Staging;
            /// @brief Byte size of the subresource.
            usize Bytes = 0;
            /// @brief The mip level being read.
            u32 MipLevel = 0;
            /// @brief The array layer being read.
            u32 ArrayLayer = 0;
            /// @brief The access the subresource is restored to after the copy.
            AccessKind RestoreTo = AccessKind::Sample;
            /// @brief The completion to deliver the bytes to.
            function<void(std::span<const u8>)> OnComplete;
            /// @brief Whether the copy has been recorded.
            bool Staged = false;
            /// @brief The pump count at which the copy was recorded.
            u64 StagedPump = 0;
        };

        /// @brief The context staging buffers and views are created on.
        Context& m_Context;
        /// @brief Readbacks requested but not yet delivered, in request order.
        vector<Entry> m_Pending;
        /// @brief Monotonic id source for AsyncReadbackHandle.
        u64 m_NextId = 1;
        /// @brief Pumps run so far; a staged copy is readable framesInFlight pumps after its own.
        u64 m_PumpCount = 0;
        /// @brief Readbacks delivered over this pump's lifetime.
        u64 m_CompletedCount = 0;
    };
}
