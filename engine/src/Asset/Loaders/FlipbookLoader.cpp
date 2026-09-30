#include "FlipbookLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Texture.h>

#include "TextureLoader.h"

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(const AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }
    }

    AssetResult<Detail::LoadJob> FlipbookLoader::Load(AssetManager& manager,
                                                      Renderer::Context& context, TaskSystem& tasks,
                                                      TypeRegistry& types, const AssetId id,
                                                      const std::span<const u8> cooked,
                                                      const bool async) const
    {
        if (cooked.size() < sizeof(CookedFlipbookHeader))
        {
            return std::unexpected(
                Corrupt(id, "flipbook: cooked blob smaller than CookedFlipbookHeader"));
        }

        CookedFlipbookHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        if (header.Version != CookedFlipbookVersion)
        {
            return std::unexpected(
                Corrupt(id, fmt::format("flipbook: cooked version {} != CookedFlipbookVersion {}",
                                        header.Version, CookedFlipbookVersion)));
        }
        if (header.Columns == 0 || header.Rows == 0 || header.FrameCount == 0 ||
            static_cast<u64>(header.FrameCount) > static_cast<u64>(header.Columns) * header.Rows ||
            header.FrameWidth == 0 || header.FrameHeight == 0)
        {
            return std::unexpected(Corrupt(id, "flipbook: the cooked grid is empty or overfull"));
        }
        if (header.Blend > static_cast<u32>(FlipbookBlend::Additive) ||
            header.AlphaMode > static_cast<u32>(FlipbookAlpha::Opaque))
        {
            return std::unexpected(
                Corrupt(id, "flipbook: unrecognized blend or alpha-mode value in cooked header"));
        }
        if (cooked.size() < sizeof(header) + static_cast<usize>(header.TextureBytes))
        {
            return std::unexpected(
                Corrupt(id, "flipbook: cooked blob smaller than header + embedded texture"));
        }

        AssetResult<Detail::LoadJob> textureJob =
            TextureLoader{}.Load(manager, context, tasks, types, id,
                                 cooked.subspan(sizeof(header), header.TextureBytes), async);
        if (!textureJob)
        {
            return std::unexpected(textureJob.error());
        }
        const Ref<Texture> atlas = std::static_pointer_cast<Texture>(textureJob->Resource);

        FlipbookInfo info{
            .Name = fmt::format("Flipbook {}", id.Value),
            .Atlas = atlas,
            .Columns = header.Columns,
            .Rows = header.Rows,
            .FrameSize = {header.FrameWidth, header.FrameHeight},
            .Clip = {.FrameCount = header.FrameCount, .Fps = header.Fps, .Loop = header.Loop != 0},
            .Blend = static_cast<FlipbookBlend>(header.Blend),
            .AlphaMode = static_cast<FlipbookAlpha>(header.AlphaMode),
        };
        if (header.HasWorldExtent != 0)
        {
            info.WorldExtent =
                vec3(header.WorldExtent[0], header.WorldExtent[1], header.WorldExtent[2]);
        }
        if (header.HasPivot != 0)
        {
            info.Pivot = vec3(header.Pivot[0], header.Pivot[1], header.Pivot[2]);
        }
        const Ref<Flipbook> flipbook = Flipbook::Create(info);

        return Detail::LoadJob{
            .Resource = Detail::RefAny(flipbook),
            .Dependencies = {},
            .Finalize = std::move(textureJob->Finalize),
        };
    }
}
