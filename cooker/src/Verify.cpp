#include <Veng/Cook/Verify.h>

#include <Veng/Asset/Archive.h>

#include <fmt/format.h>
#include <xxhash.h>

namespace Veng::Cook
{
    namespace
    {
        // xxh3-128 of a byte range, packed into the format's ContentHash.
        ContentHash Xxh3_128(std::span<const u8> bytes)
        {
            const XXH128_hash_t h = XXH3_128bits(bytes.data(), bytes.size());
            return ContentHash{.Lo = h.low64, .Hi = h.high64};
        }

        bool Equal(ContentHash a, ContentHash b)
        {
            return a.Lo == b.Lo && a.Hi == b.Hi;
        }

        bool IsZero(ContentHash h)
        {
            return h.Lo == 0 && h.Hi == 0;
        }

        const char* TypeName(AssetType type)
        {
            switch (type)
            {
                case AssetType::Raw:          return "raw";
                case AssetType::Texture:      return "texture";
                case AssetType::Mesh:         return "mesh";
                case AssetType::Shader:       return "shader";
                case AssetType::Material:     return "material";
                case AssetType::VertexLayout: return "vertex_layout";
                case AssetType::Prefab:       return "prefab";
            }
            return "unknown";
        }
    }

    VerifyReport VerifyArchive(const path& archivePath)
    {
        VerifyReport report;

        const Result<ArchiveReader> readerResult = ArchiveReader::Open(archivePath);
        if (!readerResult)
        {
            report.OpenError = readerResult.error();
            return report;
        }

        const ArchiveReader& reader = *readerResult;

        // Re-hash each blob; a zero stored hash counts as a mismatch.
        report.Assets.reserve(reader.Entries().size());
        for (const ArchiveTocEntry& entry : reader.Entries())
        {
            VerifiedAsset asset{.Id = entry.Id, .Type = entry.Type, .Ok = false};

            const optional<ArchiveEntry> blob = reader.Find(entry.Id);
            if (blob)
            {
                const ContentHash recomputed = Xxh3_128(blob->Blob);
                asset.Ok = !IsZero(entry.Hash) && Equal(recomputed, entry.Hash);
            }

            report.Assets.push_back(asset);
        }

        // Re-hash the TOC bytes to catch TOC-level tampering that blob re-hashing alone cannot detect.
        const ContentHash stored = reader.ArchiveDigest();
        const ContentHash recomputed = Xxh3_128(reader.TocBytes());
        report.DigestOk = !IsZero(stored) && Equal(recomputed, stored);

        return report;
    }

    int VerifyArchiveCli(const path& archivePath)
    {
        const VerifyReport report = VerifyArchive(archivePath);

        if (report.OpenError)
        {
            fmt::print(stderr, "vengc verify: {}: {}\n", archivePath.string(), *report.OpenError);
            return 1;
        }

        usize mismatches = 0;
        for (const VerifiedAsset& asset : report.Assets)
        {
            fmt::print("{:<8} {:>5}  0x{:016X}\n",
                asset.Ok ? "OK" : "MISMATCH", TypeName(asset.Type), asset.Id.Value);
            if (!asset.Ok)
                ++mismatches;
        }

        fmt::print("{:<8} archive digest\n", report.DigestOk ? "OK" : "MISMATCH");

        if (report.Ok())
        {
            fmt::print("verify: {} assets OK, digest OK\n", report.Assets.size());
            return 0;
        }

        fmt::print(stderr, "verify: {} of {} assets mismatched{}\n",
            mismatches, report.Assets.size(),
            report.DigestOk ? "" : ", archive digest mismatched");
        return 1;
    }
}
