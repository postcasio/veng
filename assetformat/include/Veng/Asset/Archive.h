#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/Types.h>

#include <span>

// The .vengpack archive container:
//
//   Header
//     char        magic[8]        "VENGPACK"
//     u32         version
//     u32         count           // number of TOC entries
//     ContentHash archiveDigest   // xxh3-128 over the serialized TOC bytes
//   TOC[count]                    // sorted by AssetId for binary-search lookup
//     u64         id              // AssetId
//     u32         type            // AssetType
//     u32         flags           // reserved (0)
//     u64         offset          // from start of blob region
//     u64         size
//     ContentHash hash            // xxh3-128 over this entry's blob bytes
//   Blob region
//     concatenated cooked blobs, each opaque to the container, typed by its
//     TOC entry
//
// The format assumes the cook host and run host share endianness and struct
// alignment.

namespace Veng
{
    // The container format version written by ArchiveWriter and checked by
    // ArchiveReader. Bump on any layout change; a mismatch is a clean
    // VersionMismatch error (see AssetError.h), not a crash.
    inline constexpr u32 ArchiveFormatVersion = 2;

    // A 128-bit content hash, stored raw. assetformat carries these bytes; the
    // cooker and the verify tool compute them (xxh3-128). Zero means "unhashed".
    struct ContentHash
    {
        u64 Lo = 0;
        u64 Hi = 0;
    };

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
        ContentHash Hash;
    };

    // Builds a .vengpack archive in memory: Add() each asset, then Build() (a
    // byte buffer) or Write() (a file). The TOC is emitted sorted by AssetId so
    // ArchiveReader can binary-search it.
    class ArchiveWriter
    {
    public:
        ArchiveWriter() = default;

        // hash is the caller-computed xxh3-128 of blob (cooker-side);
        // assetformat stores it raw and computes nothing. Defaulted so the
        // non-cooker callers (unit-test fixtures) compile unchanged — a zero
        // hash is "unhashed", which the format defines. Only the cooker passes
        // a real hash.
        void Add(AssetId id, AssetType type, std::span<const u8> blob, ContentHash hash = {});

        // The table-of-contents digest the caller computed over the serialized
        // TOC bytes. assetformat stores it in the header without computing one.
        void SetArchiveDigest(ContentHash digest);

        [[nodiscard]] vector<u8> Build() const;
        [[nodiscard]] VoidResult Write(const path& filePath) const;

    private:
        struct Entry
        {
            AssetId Id;
            AssetType Type;
            vector<u8> Blob;
            ContentHash Hash;
        };

        vector<Entry> m_Entries;
        ContentHash m_ArchiveDigest;
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

        // The header's table-of-contents digest, as stored — assetformat does
        // not recompute or verify it.
        [[nodiscard]] ContentHash ArchiveDigest() const { return m_ArchiveDigest; }

        // The raw serialized TOC byte region (the contiguous run of on-disk TOC
        // entries between the header and the blob region), so a verify tool can
        // recompute the digest over the exact on-disk bytes.
        [[nodiscard]] std::span<const u8> TocBytes() const;

    private:
        struct InternalTocEntry
        {
            AssetId Id;
            AssetType Type;
            u64 Offset = 0; // absolute offset into m_Storage
            u64 Size = 0;
            ContentHash Hash;
        };

        ArchiveReader() = default;

        vector<u8> m_Storage;
        vector<InternalTocEntry> m_Toc;    // sorted by Id, for Find()'s binary search
        vector<ArchiveTocEntry> m_Entries; // same order, public view
        ContentHash m_ArchiveDigest;
        usize m_TocOffset = 0; // start of the serialized TOC byte region in m_Storage
        usize m_TocSize = 0;   // its length in bytes
    };
}
