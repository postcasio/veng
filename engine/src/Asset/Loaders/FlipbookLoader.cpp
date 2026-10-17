#include "FlipbookLoader.h"

#include <array>
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

    AssetResult<Detail::ParsedAsset> FlipbookLoader::Parse(const AssetParseContext& context,
                                                           const AssetId id,
                                                           const std::span<const u8> cooked) const
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
            header.AlphaMode > static_cast<u32>(FlipbookAlpha::Opaque) ||
            header.Shading > static_cast<u32>(FlipbookShading::SixWay) ||
            header.FrameBlend > static_cast<u32>(FlipbookFrameBlend::Motion) ||
            header.TemperatureEncoding > static_cast<u32>(FlipbookTemperatureEncoding::Sqrt))
        {
            return std::unexpected(
                Corrupt(id, "flipbook: unrecognized enumerated value in cooked header"));
        }
        if (header.EmissionKeyCount > CookedFlipbookMaxEmissionKeys)
        {
            return std::unexpected(
                Corrupt(id, fmt::format("flipbook: {} emission keys exceed the {} a header holds",
                                        header.EmissionKeyCount, CookedFlipbookMaxEmissionKeys)));
        }

        constexpr u32 PrimarySlot = 0;
        constexpr u32 LightBSlot = 1;
        constexpr u32 MotionSlot = 2;
        constexpr u32 TemperatureSlot = 3;
        const auto present = [&header](const u32 slot) { return header.TextureBytes[slot] != 0; };
        const bool sixWay = header.Shading == static_cast<u32>(FlipbookShading::SixWay);
        if (!present(PrimarySlot) || present(LightBSlot) != sixWay ||
            (present(TemperatureSlot) && !sixWay) ||
            present(MotionSlot) != (header.MotionScale > 0.0f))
        {
            return std::unexpected(
                Corrupt(id, "flipbook: the cooked texture slots do not match the header"));
        }

        usize offset = sizeof(header);
        vector<f32> frameScale;
        if (header.HasFrameScale != 0)
        {
            const usize scaleBytes = static_cast<usize>(header.FrameCount) * sizeof(f32);
            if (cooked.size() < offset + scaleBytes)
            {
                return std::unexpected(
                    Corrupt(id, "flipbook: cooked blob smaller than header + frame scales"));
            }
            frameScale.resize(header.FrameCount);
            std::memcpy(frameScale.data(), cooked.data() + offset, scaleBytes);
            offset += scaleBytes;
            for (const f32 scale : frameScale)
            {
                if (!(scale > 0.0f && scale <= 1.0f))
                {
                    return std::unexpected(
                        Corrupt(id, "flipbook: a cooked frame scale lies outside (0, 1]"));
                }
            }
        }

        u64 textureBytes = 0;
        for (const u32 bytes : header.TextureBytes)
        {
            textureBytes += bytes;
        }
        if (cooked.size() < offset + textureBytes)
        {
            return std::unexpected(
                Corrupt(id, "flipbook: cooked blob smaller than header + embedded textures"));
        }

        std::array<Ref<Texture>, CookedFlipbookTextureSlots> textures;
        vector<function<VoidResult()>> finalizers;
        for (u32 slot = 0; slot < CookedFlipbookTextureSlots; ++slot)
        {
            if (!present(slot))
            {
                continue;
            }
            AssetResult<Detail::LoadJob> textureJob = TextureLoader::PrepareTexture(
                context, id, cooked.subspan(offset, header.TextureBytes[slot]));
            if (!textureJob)
            {
                return std::unexpected(textureJob.error());
            }
            offset += header.TextureBytes[slot];
            textures[slot] = std::static_pointer_cast<Texture>(textureJob->Resource);
            if (textures[slot]->GetExtent() != textures[PrimarySlot]->GetExtent())
            {
                return std::unexpected(
                    Corrupt(id, "flipbook: a cooked texture's extent is not the atlas's"));
            }
            if (textureJob->Finalize)
            {
                finalizers.push_back(std::move(textureJob->Finalize));
            }
        }

        FlipbookInfo info{
            .Name = fmt::format("Flipbook {}", id.Value),
            .Atlas = textures[PrimarySlot],
            .Columns = header.Columns,
            .Rows = header.Rows,
            .FrameSize = {header.FrameWidth, header.FrameHeight},
            .Clip = {.FrameCount = header.FrameCount, .Fps = header.Fps, .Loop = header.Loop != 0},
            .Blend = static_cast<FlipbookBlend>(header.Blend),
            .AlphaMode = static_cast<FlipbookAlpha>(header.AlphaMode),
            .Shading = static_cast<FlipbookShading>(header.Shading),
            .LightB = textures[LightBSlot],
            .Motion = textures[MotionSlot],
            .Temperature = textures[TemperatureSlot],
            .MotionScale = header.MotionScale,
            .TemperatureEncoding =
                static_cast<FlipbookTemperatureEncoding>(header.TemperatureEncoding),
            .FrameBlend = static_cast<FlipbookFrameBlend>(header.FrameBlend),
            .FrameScale = std::move(frameScale),
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
        info.Emission.Ignition = header.Ignition;
        for (u32 key = 0; key < header.EmissionKeyCount; ++key)
        {
            const f32* values = header.EmissionKeys[key];
            if (key > 0 && !(values[0] > header.EmissionKeys[key - 1][0]))
            {
                return std::unexpected(
                    Corrupt(id, "flipbook: the cooked emission keys are out of order"));
            }
            info.Emission.Ramp.push_back(
                {.T = values[0], .Color = vec3(values[1], values[2], values[3])});
        }
        const Ref<Flipbook> flipbook = Flipbook::Create(info);

        // Each texture registers in the bindless registry in its own finalize; the first failure
        // stops the rest.
        function<VoidResult()> finalize;
        if (!finalizers.empty())
        {
            finalize = [finalizers = std::move(finalizers)]() -> VoidResult
            {
                for (const function<VoidResult()>& step : finalizers)
                {
                    if (const VoidResult result = step(); !result)
                    {
                        return result;
                    }
                }
                return {};
            };
        }

        return Detail::ParsedJob(Detail::LoadJob{
            .Resource = Detail::RefAny(flipbook),
            .Dependencies = {},
            .Finalize = std::move(finalize),
        });
    }
}
