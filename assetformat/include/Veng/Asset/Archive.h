#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/Types.h>

#include <span>

// The .vengpack archive container:
//
//   Header
//     char  magic[8]   "VENGPACK"
//     u32   version
//     u32   count                 // number of TOC entries
//   TOC[count]                    // sorted by AssetId for binary-search lookup
//     u64   id                    // AssetId
//     u32   type                  // AssetType
//     u32   flags                 // reserved (0)
//     u64   offset                // from start of blob region
//     u64   size
//   Blob region
//     concatenated cooked blobs, each opaque to the container, typed by its
//     TOC entry
//
// v1 assumes the cook host and run host share endianness and struct alignment.

namespace Veng
{
    // The container format version written by ArchiveWriter and checked by
    // ArchiveReader. Bump on any layout change; a mismatch is a clean
    // VersionMismatch error (see AssetError.h), not a crash.
    inline constexpr u32 ArchiveFormatVersion = 1;

    // One asset as read back from an archive: its id, type, and a view of its
    // cooked blob bytes within the reader's storage. Valid only for the
    // lifetime of the ArchiveReader that returned it.
    struct ArchiveEntry
    {
        AssetId Id;
        AssetType Type;
        std::span<const u8> Blob;
    };

    // One archive TOC entry, for tooling/inspection (no blob data — use Find()
    // to fetch bytes).
    struct ArchiveTocEntry
    {
        AssetId Id;
        AssetType Type;
        u64 Size = 0;
    };

    // Builds a .vengpack archive in memory: Add() each asset, then Build() (a
    // byte buffer) or Write() (a file). The TOC is emitted sorted by AssetId so
    // ArchiveReader can binary-search it.
    class ArchiveWriter
    {
    public:
        ArchiveWriter() = default;

        void Add(AssetId id, AssetType type, std::span<const u8> blob);

        [[nodiscard]] vector<u8> Build() const;
        [[nodiscard]] VoidResult Write(const path& filePath) const;

    private:
        struct Entry
        {
            AssetId Id;
            AssetType Type;
            vector<u8> Blob;
        };

        vector<Entry> m_Entries;
    };

    // Reads a .vengpack archive: Open() a file or FromBytes() an in-memory
    // buffer (copied into the reader's own storage). Find() resolves an
    // AssetId via binary search over the id-sorted TOC; Entries() exposes the
    // TOC for tooling/inspection.
    class ArchiveReader
    {
    public:
        [[nodiscard]] static Result<ArchiveReader> Open(const path& filePath);
        [[nodiscard]] static Result<ArchiveReader> FromBytes(std::span<const u8> bytes);

        [[nodiscard]] optional<ArchiveEntry> Find(AssetId id) const;
        [[nodiscard]] const vector<ArchiveTocEntry>& Entries() const { return m_Entries; }

    private:
        struct InternalTocEntry
        {
            AssetId Id;
            AssetType Type;
            u64 Offset = 0; // absolute offset into m_Storage
            u64 Size = 0;
        };

        ArchiveReader() = default;

        vector<u8> m_Storage;
        vector<InternalTocEntry> m_Toc;    // sorted by Id, for Find()'s binary search
        vector<ArchiveTocEntry> m_Entries; // same order, public view
    };
}
