#include <Veng/Asset/Archive.h>

#include <algorithm>
#include <cstring>
#include <fstream>

#include <fmt/format.h>

namespace Veng
{
    namespace
    {
        // On-disk header and TOC entry layouts. Plain structs with no padding
        // (verified by the static_asserts below) so they can be memcpy'd
        // directly to/from the archive buffer.
        struct OnDiskHeader
        {
            char Magic[8];
            u32 Version;
            u32 Count;
            u64 DigestLo; // ContentHash over the serialized TOC bytes
            u64 DigestHi;
        };
        static_assert(sizeof(OnDiskHeader) == 32);

        struct OnDiskTocEntry
        {
            u64 Id;
            u32 Type;
            u32 Flags;
            u64 Offset;
            u64 Size;
            u64 HashLo; // ContentHash over this entry's blob bytes
            u64 HashHi;
        };
        static_assert(sizeof(OnDiskTocEntry) == 48);

        constexpr char ArchiveMagic[8] = {'V', 'E', 'N', 'G', 'P', 'A', 'C', 'K'};
    }

    void ArchiveWriter::Add(AssetId id, AssetType type, std::span<const u8> blob, ContentHash hash)
    {
        m_Entries.push_back(Entry{
            .Id = id,
            .Type = type,
            .Blob = vector<u8>(blob.begin(), blob.end()),
            .Hash = hash,
        });
    }

    void ArchiveWriter::SetArchiveDigest(ContentHash digest)
    {
        m_ArchiveDigest = digest;
    }

    vector<u8> ArchiveWriter::Build() const
    {
        vector<const Entry*> sorted;
        sorted.reserve(m_Entries.size());
        for (const Entry& entry : m_Entries)
            sorted.push_back(&entry);

        std::sort(sorted.begin(), sorted.end(),
                  [](const Entry* a, const Entry* b) { return a->Id.Value < b->Id.Value; });

        OnDiskHeader header{};
        std::memcpy(header.Magic, ArchiveMagic, sizeof(ArchiveMagic));
        header.Version = ArchiveFormatVersion;
        header.Count = static_cast<u32>(sorted.size());
        header.DigestLo = m_ArchiveDigest.Lo;
        header.DigestHi = m_ArchiveDigest.Hi;

        vector<OnDiskTocEntry> toc;
        toc.reserve(sorted.size());

        u64 blobOffset = 0;
        for (const Entry* entry : sorted)
        {
            toc.push_back(OnDiskTocEntry{
                .Id = entry->Id.Value,
                .Type = static_cast<u32>(entry->Type),
                .Flags = 0,
                .Offset = blobOffset,
                .Size = entry->Blob.size(),
                .HashLo = entry->Hash.Lo,
                .HashHi = entry->Hash.Hi,
            });
            blobOffset += entry->Blob.size();
        }

        vector<u8> out(sizeof(OnDiskHeader) + toc.size() * sizeof(OnDiskTocEntry) + blobOffset);

        usize cursor = 0;
        std::memcpy(out.data() + cursor, &header, sizeof(header));
        cursor += sizeof(header);

        for (const OnDiskTocEntry& entry : toc)
        {
            std::memcpy(out.data() + cursor, &entry, sizeof(entry));
            cursor += sizeof(entry);
        }

        for (const Entry* entry : sorted)
        {
            std::memcpy(out.data() + cursor, entry->Blob.data(), entry->Blob.size());
            cursor += entry->Blob.size();
        }

        return out;
    }

    VoidResult ArchiveWriter::Write(const path& filePath) const
    {
        const vector<u8> bytes = Build();

        std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
        if (!file)
            return std::unexpected(fmt::format(
                "ArchiveWriter::Write: failed to open '{}' for writing", filePath.string()));

        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!file)
            return std::unexpected(
                fmt::format("ArchiveWriter::Write: failed writing to '{}'", filePath.string()));

        return {};
    }

    Result<ArchiveReader> ArchiveReader::FromBytes(std::span<const u8> bytes)
    {
        if (bytes.size() < sizeof(OnDiskHeader))
            return std::unexpected("ArchiveReader::FromBytes: buffer too small for archive header");

        OnDiskHeader header;
        std::memcpy(&header, bytes.data(), sizeof(header));

        if (std::memcmp(header.Magic, ArchiveMagic, sizeof(ArchiveMagic)) != 0)
            return std::unexpected("ArchiveReader::FromBytes: bad magic (not a .vengpack archive)");

        if (header.Version != ArchiveFormatVersion)
        {
            return std::unexpected(
                fmt::format("ArchiveReader::FromBytes: version mismatch (expected {}, got {})",
                            ArchiveFormatVersion, header.Version));
        }

        const usize tocBytes = static_cast<usize>(header.Count) * sizeof(OnDiskTocEntry);
        if (bytes.size() < sizeof(OnDiskHeader) + tocBytes)
            return std::unexpected(
                "ArchiveReader::FromBytes: buffer too small for table of contents");

        ArchiveReader reader;
        reader.m_Storage.assign(bytes.begin(), bytes.end());

        reader.m_ArchiveDigest = ContentHash{.Lo = header.DigestLo, .Hi = header.DigestHi};
        reader.m_TocOffset = sizeof(OnDiskHeader);
        reader.m_TocSize = tocBytes;

        const usize blobRegionStart = sizeof(OnDiskHeader) + tocBytes;

        reader.m_Toc.reserve(header.Count);
        reader.m_Entries.reserve(header.Count);

        for (u32 i = 0; i < header.Count; ++i)
        {
            OnDiskTocEntry onDisk;
            std::memcpy(&onDisk,
                        reader.m_Storage.data() + sizeof(OnDiskHeader) +
                            static_cast<usize>(i) * sizeof(OnDiskTocEntry),
                        sizeof(onDisk));

            if (onDisk.Offset > reader.m_Storage.size() - blobRegionStart ||
                onDisk.Size > reader.m_Storage.size() - blobRegionStart - onDisk.Offset)
            {
                return std::unexpected(
                    "ArchiveReader::FromBytes: TOC entry blob range out of bounds");
            }

            const AssetId id{.Value = onDisk.Id};
            const auto type = static_cast<AssetType>(onDisk.Type);
            const ContentHash hash{.Lo = onDisk.HashLo, .Hi = onDisk.HashHi};

            reader.m_Toc.push_back(InternalTocEntry{
                .Id = id,
                .Type = type,
                .Offset = blobRegionStart + onDisk.Offset,
                .Size = onDisk.Size,
                .Hash = hash,
            });

            reader.m_Entries.push_back(ArchiveTocEntry{
                .Id = id,
                .Type = type,
                .Size = onDisk.Size,
                .Hash = hash,
            });
        }

        return reader;
    }

    Result<ArchiveReader> ArchiveReader::Open(const path& filePath)
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file)
            return std::unexpected(
                fmt::format("ArchiveReader::Open: failed to open '{}'", filePath.string()));

        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        if (size < 0)
            return std::unexpected(fmt::format(
                "ArchiveReader::Open: failed to determine size of '{}'", filePath.string()));

        file.seekg(0, std::ios::beg);

        vector<u8> bytes(static_cast<usize>(size));
        file.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!file)
            return std::unexpected(
                fmt::format("ArchiveReader::Open: failed reading '{}'", filePath.string()));

        return FromBytes(bytes);
    }

    optional<ArchiveEntry> ArchiveReader::Find(AssetId id) const
    {
        const auto it = std::lower_bound(m_Toc.begin(), m_Toc.end(), id,
                                         [](const InternalTocEntry& entry, AssetId value)
                                         { return entry.Id.Value < value.Value; });

        if (it == m_Toc.end() || it->Id.Value != id.Value)
            return std::nullopt;

        return ArchiveEntry{
            .Id = it->Id,
            .Type = it->Type,
            .Blob = std::span<const u8>(m_Storage.data() + it->Offset, it->Size),
        };
    }

    std::span<const u8> ArchiveReader::TocBytes() const
    {
        return std::span<const u8>(m_Storage.data() + m_TocOffset, m_TocSize);
    }
}
