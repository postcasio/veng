#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/Types.h>

#include <map>
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
//     u32         codec           // ArchiveCodec: 0 = stored, 1 = zstd
//     u64         offset          // from start of blob region
//     u64         size            // stored (on-disk) blob byte length
//     ContentHash hash            // xxh3-128 over this entry's stored bytes
//     u64         uncompressedSize // inflated blob byte length (== size when stored)
//   Blob region
//     concatenated stored (possibly compressed) cooked blobs, each opaque to
//     the container, typed by its TOC entry
//
// The format assumes the cook host and run host share endianness and struct
// alignment.

namespace Veng
{
    /// @brief The container format version written by ArchiveWriter and checked by ArchiveReader.
    ///
    /// A mismatch produces a clean VersionMismatch error (see AssetError.h), not a crash.
    /// Bump on any layout change.
    inline constexpr u32 ArchiveFormatVersion = 5;

    /// @brief How a blob is stored in the archive's blob region.
    ///
    /// The cooker picks the codec per blob (the smaller of stored-vs-compressed) and the
    /// reader inflates a Zstd blob on resolve. assetpack interprets the codec but never
    /// chooses one — that is the cooker's call.
    enum class ArchiveCodec : u32
    {
        /// @brief The blob's stored bytes are its exact contents; resolve is zero-copy.
        Stored = 0,
        /// @brief The blob's stored bytes are zstd-compressed; resolve inflates them.
        Zstd = 1,
    };

    /// @brief A 128-bit content hash stored as raw bytes.
    ///
    /// The cooker and verify tool compute these (xxh3-128); assetpack stores them without
    /// interpreting them. Zero means "unhashed".
    struct ContentHash
    {
        /// @brief Low 64 bits of the hash.
        u64 Lo = 0;
        /// @brief High 64 bits of the hash.
        u64 Hi = 0;
    };

    /// @brief One asset as read back from an archive: its id, type, and a view of its blob bytes.
    ///
    /// Valid only for the lifetime of the ArchiveReader that returned it.
    struct ArchiveEntry
    {
        /// @brief The asset's unique identifier.
        AssetId Id;
        /// @brief The asset type, matching the TOC entry's type field.
        AssetType Type;
        /// @brief View into the reader's storage; valid for the reader's lifetime.
        std::span<const u8> Blob;
    };

    /// @brief One archive TOC entry for tooling and inspection (no blob data).
    ///
    /// Use ArchiveReader::Find() to fetch blob bytes for a given id.
    struct ArchiveTocEntry
    {
        /// @brief The asset's unique identifier.
        AssetId Id;
        /// @brief The asset type.
        AssetType Type;
        /// @brief Codec the blob is stored under.
        ArchiveCodec Codec = ArchiveCodec::Stored;
        /// @brief Stored (on-disk) byte size of the cooked blob.
        u64 Size = 0;
        /// @brief Inflated byte size of the blob; equals Size when stored uncompressed.
        u64 UncompressedSize = 0;
        /// @brief xxh3-128 content hash of the stored (on-disk) bytes; zero if unhashed.
        ContentHash Hash;
    };

    /// @brief Builds a .vengpack archive in memory.
    ///
    /// Call Add() for each asset, then Build() to obtain a byte buffer or Write() to emit a file.
    /// The TOC is emitted sorted by AssetId so ArchiveReader can binary-search it.
    class ArchiveWriter
    {
    public:
        /// @brief Constructs an empty writer.
        ArchiveWriter() = default;

        /// @brief Adds an asset blob to the archive.
        ///
        /// The blob bytes are stored verbatim — the writer compresses nothing. The cooker
        /// compresses a blob itself, picks the smaller of stored-vs-compressed, and passes the
        /// resulting codec and the original (inflated) length here; a stored blob passes the
        /// defaults.
        /// @param id                The asset identifier.
        /// @param type              The asset type.
        /// @param blob              The stored blob bytes; copied into the writer's storage.
        /// @param hash              The caller-computed xxh3-128 of the stored bytes. Defaulted to
        ///                          zero ("unhashed") so non-cooker callers (e.g. unit-test
        ///                          fixtures) compile unchanged; only the cooker passes a real hash.
        /// @param codec             How the blob bytes are stored. Defaulted to Stored so
        ///                          non-cooker callers compile unchanged.
        /// @param uncompressedSize  The inflated length of the blob. Defaulted to the blob's stored
        ///                          size, which is correct for a Stored blob; a Zstd caller passes
        ///                          the original length.
        void Add(AssetId id, AssetType type, std::span<const u8> blob, ContentHash hash = {},
                 ArchiveCodec codec = ArchiveCodec::Stored,
                 optional<u64> uncompressedSize = std::nullopt);

        /// @brief Sets the table-of-contents digest stored in the archive header.
        ///
        /// assetpack stores the digest without computing one; the cooker computes it over the
        /// serialized TOC bytes and passes it here.
        /// @param digest  The caller-computed xxh3-128 digest over the serialized TOC.
        void SetArchiveDigest(ContentHash digest);

        /// @brief Serializes the archive to a byte buffer.
        /// @return The complete archive bytes, suitable for writing to disk or passing to ArchiveReader::FromBytes().
        [[nodiscard]] vector<u8> Build() const;

        /// @brief Writes the serialized archive to a file.
        /// @param filePath  Destination path; the file is truncated if it already exists.
        /// @return An error string on failure.
        [[nodiscard]] VoidResult Write(const path& filePath) const;

    private:
        /// @brief One pending entry before the archive is built.
        struct Entry
        {
            /// @brief Asset identifier.
            AssetId Id;
            /// @brief Asset type.
            AssetType Type;
            /// @brief Stored blob bytes.
            vector<u8> Blob;
            /// @brief xxh3-128 content hash of the stored bytes; zero if unhashed.
            ContentHash Hash;
            /// @brief Codec the stored bytes are under.
            ArchiveCodec Codec = ArchiveCodec::Stored;
            /// @brief Inflated blob length.
            u64 UncompressedSize = 0;
        };

        /// @brief Pending entries, in insertion order.
        vector<Entry> m_Entries;
        /// @brief TOC digest set via SetArchiveDigest; zero until set.
        ContentHash m_ArchiveDigest;
    };

    /// @brief Reads a .vengpack archive from a file or an in-memory buffer.
    ///
    /// Open() reads a file; FromBytes() accepts an in-memory span (copied into the reader's own
    /// storage). Find() resolves an AssetId via binary search over the id-sorted TOC.
    /// Entries() exposes the full TOC for tooling and inspection.
    class ArchiveReader
    {
    public:
        /// @brief Opens and parses a .vengpack file.
        /// @param filePath  Path to the archive file.
        /// @return The reader on success, or an error string on failure.
        [[nodiscard]] static Result<ArchiveReader> Open(const path& filePath);

        /// @brief Parses a .vengpack archive from an in-memory byte span.
        ///
        /// The bytes are copied into the reader's own storage; the caller's buffer need not outlive
        /// this call.
        /// @param bytes  The raw archive bytes.
        /// @return The reader on success, or an error string on failure.
        [[nodiscard]] static Result<ArchiveReader> FromBytes(std::span<const u8> bytes);

        /// @brief Looks up an asset by id using binary search over the id-sorted TOC.
        ///
        /// A stored entry returns a zero-copy span into the reader's backing storage. A
        /// zstd entry is inflated on first resolve into a reader-owned, id-keyed cache and the
        /// span points into that cache; a later resolve of the same id reuses the cached bytes.
        /// Cache entries are never evicted, so every span stays valid for the reader's lifetime —
        /// including across a later inflating resolve of a different id.
        /// @param id  The asset id to find.
        /// @return The entry (blob view into the reader's storage or inflate cache) if found, or
        ///         nullopt.
        /// @warning Find is not thread-safe: the lazy inflate mutates the reader's cache. Resolve
        ///          on a single thread (the render thread, as every loader does).
        [[nodiscard]] optional<ArchiveEntry> Find(AssetId id) const;

        /// @brief Looks up an asset by id and returns a view of its stored (on-disk) bytes.
        ///
        /// Unlike Find(), this never inflates a zstd entry — it returns the compressed bytes as
        /// they sit in the blob region. A verify tool re-hashes these against the TOC's stored
        /// hash with no decode dependency.
        /// @param id  The asset id to find.
        /// @return The stored blob view (zero-copy into the reader's storage) if found, or nullopt.
        [[nodiscard]] optional<ArchiveEntry> FindStored(AssetId id) const;

        /// @brief Returns the full TOC for tooling and inspection.
        /// @return The table-of-contents entries in ascending AssetId order.
        [[nodiscard]] const vector<ArchiveTocEntry>& Entries() const { return m_Entries; }

        /// @brief Returns the header's table-of-contents digest as stored.
        ///
        /// assetpack does not recompute or verify it; use vengc verify for that.
        /// @return The stored xxh3-128 digest over the serialized TOC bytes.
        [[nodiscard]] ContentHash ArchiveDigest() const { return m_ArchiveDigest; }

        /// @brief Returns the raw serialized TOC byte region from the archive.
        ///
        /// This is the contiguous on-disk region between the header and the blob region,
        /// so a verify tool can recompute the digest over the exact on-disk bytes.
        /// @return A span into the reader's internal storage; valid for the reader's lifetime.
        [[nodiscard]] std::span<const u8> TocBytes() const;

    private:
        /// @brief Internal TOC entry carrying the absolute blob offset within m_Storage.
        struct InternalTocEntry
        {
            /// @brief Asset identifier.
            AssetId Id;
            /// @brief Asset type.
            AssetType Type;
            /// @brief Absolute byte offset of the stored blob within m_Storage.
            u64 Offset = 0;
            /// @brief Stored (on-disk) blob byte size.
            u64 Size = 0;
            /// @brief Inflated blob byte size; equals Size when stored uncompressed.
            u64 UncompressedSize = 0;
            /// @brief Codec the stored bytes are under.
            ArchiveCodec Codec = ArchiveCodec::Stored;
            /// @brief xxh3-128 content hash of the stored bytes.
            ContentHash Hash;
        };

        ArchiveReader() = default;

        /// @brief Backing store for all parsed archive bytes.
        vector<u8> m_Storage;
        /// @brief TOC sorted by Id; used by Find() for binary search.
        vector<InternalTocEntry> m_Toc;
        /// @brief Inflated bytes of resolved zstd entries, keyed by id.
        ///
        /// std::map gives stable value addresses, so a span returned from an earlier Find()
        /// stays valid when a later Find() inflates a different entry. Populated lazily by Find()
        /// (hence mutable on a const method) and never evicted.
        mutable std::map<AssetId, vector<u8>> m_InflateCache;
        /// @brief Public view of the TOC in the same order as m_Toc.
        vector<ArchiveTocEntry> m_Entries;
        /// @brief Stored archive digest (from the header).
        ContentHash m_ArchiveDigest;
        /// @brief Byte offset of the serialized TOC region within m_Storage.
        usize m_TocOffset = 0;
        /// @brief Byte length of the serialized TOC region.
        usize m_TocSize = 0;
    };
}
