#include <Veng/Renderer/AsyncReadback.h>

#include <algorithm>

#include <Veng/Log.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/FormatInfo.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>

namespace Veng::Renderer
{
    namespace
    {
        // The view type a single-subresource view of an image takes: a volume level is read whole
        // (its z slices are part of the level), everything else one 2D layer at a time.
        ImageViewType SubresourceViewType(const ImageType type)
        {
            return type == ImageType::Type3D ? ImageViewType::Type3D : ImageViewType::Type2D;
        }
    }

    AsyncReadback::AsyncReadback(Context& context) : m_Context(context) {}

    AsyncReadback::~AsyncReadback() = default;

    AsyncReadbackHandle AsyncReadback::Request(AsyncReadbackRequest request)
    {
        if (!request.Image)
        {
            Log::Error("AsyncReadback::Request called with no image");
            return {};
        }

        const Ref<Veng::Renderer::Image>& image = request.Image;
        if (request.MipLevel >= image->GetMipLevels() || request.ArrayLayer >= image->GetLayers())
        {
            Log::Error("AsyncReadback::Request: '{}' has no mip {} layer {}", image->GetName(),
                       request.MipLevel, request.ArrayLayer);
            return {};
        }

        const uvec3 extent = image->GetExtent();
        const u32 width = std::max(1u, extent.x >> request.MipLevel);
        const u32 height = std::max(1u, extent.y >> request.MipLevel);
        const u32 depth = std::max(1u, extent.z >> request.MipLevel);
        const usize bytes = BytesForLevel(image->GetFormat(), width, height) * depth;
        if (bytes == 0)
        {
            Log::Error("AsyncReadback::Request: '{}' has a format with no known byte size",
                       image->GetName());
            return {};
        }

        Entry entry{
            .Id = m_NextId++,
            .Image = image,
            .View = ImageView::Create(m_Context,
                                      {
                                          .Name = request.Name + " (View)",
                                          .Image = image,
                                          .ViewType = SubresourceViewType(image->GetType()),
                                          .BaseMipLevel = request.MipLevel,
                                          .MipLevels = 1,
                                          .BaseArrayLayer = request.ArrayLayer,
                                          .ArrayLayers = 1,
                                      }),
            .Staging = Buffer::Create(m_Context,
                                      {
                                          .Name = request.Name + " (Staging)",
                                          .Size = bytes,
                                          .Usage = BufferUsage::TransferDst,
                                          .HostMapped = true,
                                      }),
            .Bytes = bytes,
            .MipLevel = request.MipLevel,
            .ArrayLayer = request.ArrayLayer,
            .RestoreTo = request.RestoreTo,
            .OnComplete = std::move(request.OnComplete),
        };

        const AsyncReadbackHandle handle{.Id = entry.Id};
        m_Pending.push_back(std::move(entry));
        return handle;
    }

    bool AsyncReadback::Cancel(const AsyncReadbackHandle handle)
    {
        const auto it = std::ranges::find(m_Pending, handle.Id, &Entry::Id);
        if (it == m_Pending.end())
        {
            return false;
        }
        m_Pending.erase(it);
        return true;
    }

    void AsyncReadback::Pump(CommandBuffer& cmd)
    {
        m_PumpCount++;

        // Deliver first: a copy staged framesInFlight pumps ago rode a frame whose fence
        // AcquireNextFrame has since waited, so the mapped bytes are the finished GPU result. The
        // completions run before the new stagings so a readback requested from one is staged this
        // frame rather than the next.
        const u64 latency = m_Context.GetMaxFramesInFlight();
        vector<Entry> ready;
        vector<Entry> stillPending;
        stillPending.reserve(m_Pending.size());
        for (Entry& entry : m_Pending)
        {
            if (entry.Staged && m_PumpCount - entry.StagedPump >= latency)
            {
                ready.push_back(std::move(entry));
            }
            else
            {
                stillPending.push_back(std::move(entry));
            }
        }
        m_Pending = std::move(stillPending);

        for (const Entry& entry : ready)
        {
            m_CompletedCount++;
            if (entry.OnComplete)
            {
                const auto* bytes = static_cast<const u8*>(entry.Staging->GetMappedData());
                entry.OnComplete(std::span<const u8>(bytes, entry.Bytes));
            }
        }

        for (Entry& entry : m_Pending)
        {
            if (entry.Staged)
            {
                continue;
            }
            cmd.PrepareForAccess(entry.View, AccessKind::TransferSrc);
            cmd.CopyImageSubresourceToBuffer(entry.Image, entry.Staging, entry.MipLevel,
                                             entry.ArrayLayer);
            cmd.PrepareForAccess(entry.View, entry.RestoreTo);
            entry.Staged = true;
            entry.StagedPump = m_PumpCount;
        }
    }
}
