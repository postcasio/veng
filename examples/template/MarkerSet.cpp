#include "MarkerSet.h"

#include <Veng/Asset/AssetError.h>

#include <fmt/format.h>

#include <algorithm>
#include <cstring>

namespace Template
{
    using namespace Veng;

    const Marker* MarkerSet::Find(const string_view name) const
    {
        const auto it =
            std::ranges::find_if(Markers, [name](const Marker& m) { return m.Name == name; });
        return it != Markers.end() ? &*it : nullptr;
    }

    AssetResult<Detail::LoadJob> MarkerSetLoader::Load(AssetManager&, Renderer::Context&,
                                                       TaskSystem&, TypeRegistry&, const AssetId id,
                                                       const std::span<const u8> cooked, bool) const
    {
        // A cooked blob is a build artifact, not untrusted input — but a stale or truncated one is
        // recoverable, so every bound is checked and reported rather than asserted.
        const auto fail = [id](string detail)
        {
            return std::unexpected(AssetLoadError{
                .Kind = AssetError::LoadFailed, .Id = id, .Detail = std::move(detail)});
        };

        if (cooked.size() < sizeof(CookedMarkerSetHeader))
        {
            return fail("marker set blob is shorter than its header");
        }

        CookedMarkerSetHeader header{};
        std::memcpy(&header, cooked.data(), sizeof(header));

        if (header.Magic != MarkerSetMagic)
        {
            return fail(fmt::format("marker set blob has magic {:#010X}, expected {:#010X}",
                                    header.Magic, MarkerSetMagic));
        }
        if (header.Version != MarkerSetVersion)
        {
            return fail(fmt::format("marker set blob is version {}, expected {}", header.Version,
                                    MarkerSetVersion));
        }

        const usize recordBytes = usize{header.MarkerCount} * sizeof(CookedMarkerRecord);
        const usize expected = sizeof(header) + recordBytes + usize{header.NameHeapBytes};
        if (cooked.size() < expected)
        {
            return fail(fmt::format("marker set blob is {} bytes, its header declares {}",
                                    cooked.size(), expected));
        }

        const u8* const records = cooked.data() + sizeof(header);
        const char* const nameHeap = reinterpret_cast<const char*>(records + recordBytes);

        auto set = CreateRef<MarkerSet>();
        set->Markers.reserve(header.MarkerCount);
        for (u32 i = 0; i < header.MarkerCount; ++i)
        {
            CookedMarkerRecord record{};
            std::memcpy(&record, records + (usize{i} * sizeof(record)), sizeof(record));

            if (usize{record.NameOffset} + record.NameLength > header.NameHeapBytes)
            {
                return fail(fmt::format("marker {}'s name runs past the blob's name heap", i));
            }

            set->Markers.push_back(Marker{
                .Name = string(nameHeap + record.NameOffset, record.NameLength),
                .Position = vec3(record.Position[0], record.Position[1], record.Position[2])});
        }

        return Detail::LoadJob{
            .Resource = std::static_pointer_cast<void>(std::move(set)),
            .Dependencies = {},
            .Finalize = {},
        };
    }
}
