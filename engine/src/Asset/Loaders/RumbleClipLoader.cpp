#include "RumbleClipLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Haptics/RumbleClip.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }
    }

    AssetResult<Detail::ParsedAsset> RumbleClipLoader::Parse(const AssetParseContext& parse,
                                                             const AssetId id,
                                                             const std::span<const u8> cooked) const
    {
        if (cooked.size() < sizeof(CookedRumbleClipHeader))
        {
            return std::unexpected(
                Corrupt(id, "rumble clip: cooked blob smaller than CookedRumbleClipHeader"));
        }

        CookedRumbleClipHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));
        if (header.Version != CookedRumbleClipVersion)
        {
            return std::unexpected(Corrupt(
                id, fmt::format("rumble clip: blob version {} does not match expected version {}",
                                header.Version, CookedRumbleClipVersion)));
        }

        const usize cursor = sizeof(CookedRumbleClipHeader);
        if (cooked.size() < cursor + header.RecordBytes)
        {
            return std::unexpected(Corrupt(id, "rumble clip: cooked blob truncated"));
        }

        const TypeRegistry& types = parse.Types;
        Haptics::RumbleClipData data;
        const VoidResult read = ReadFields(cooked.subspan(cursor, header.RecordBytes), &data,
                                           types.Info(TypeIdOf<Haptics::RumbleClipData>()), types);
        if (!read)
        {
            return std::unexpected(Corrupt(id, read.error()));
        }

        const Ref<Haptics::RumbleClip> clip = Haptics::RumbleClip::Create(std::move(data));
        return Detail::ParsedJob(Detail::LoadJob{.Resource = Detail::RefAny(clip)});
    }
}
