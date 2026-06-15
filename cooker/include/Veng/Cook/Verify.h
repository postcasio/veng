#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/Types.h>

// Archive integrity verification — the on-demand check the engine loader
// deliberately does not perform (the runtime trusts its packs). Re-hashes a
// cooked .vengpack and compares against the stored per-blob hashes and the
// table-of-contents digest. Cooker-side only: it reuses the cooker's vendored
// xxHash (xxh3-128) and never links libveng.

namespace Veng::Cook
{
    // One asset's verdict: its id/type and whether its re-hashed blob bytes
    // match the stored per-blob Hash. A zero stored hash (an archive written by
    // a non-cooker tool) counts as a mismatch, since every cooked archive is
    // fully hashed.
    struct VerifiedAsset
    {
        AssetId Id;
        AssetType Type;
        bool Ok = false;
    };

    // The outcome of verifying one archive. Ok() is the exit-code predicate:
    // true only when the archive opened, every blob matched, and the recomputed
    // TOC digest matched the stored one.
    struct VerifyReport
    {
        // Set when the archive could not be opened (unreadable, bad magic, or a
        // version mismatch). When present, no hashing ran and Assets is empty.
        optional<string> OpenError;

        vector<VerifiedAsset> Assets;

        // True only when the archive carried a non-zero digest that matched the
        // hash recomputed over TocBytes(). A zero stored digest counts as a
        // mismatch, like a zero per-blob hash.
        bool DigestOk = false;

        [[nodiscard]] bool Ok() const
        {
            if (OpenError)
                return false;
            if (!DigestOk)
                return false;
            for (const VerifiedAsset& asset : Assets)
                if (!asset.Ok)
                    return false;
            return true;
        }
    };

    // Open archivePath, re-hash every blob and the serialized TOC, and compare
    // against the stored hashes. The returned report is the whole verdict; it
    // never aborts on a corrupt archive — a mismatch is data carried in the
    // report, not a fatal error.
    [[nodiscard]] VerifyReport VerifyArchive(const path& archivePath);

    // Print VerifyArchive(archivePath)'s report (per-asset OK/MISMATCH lines and
    // a summary) and return a process exit code: 0 when clean, 1 on any mismatch,
    // unreadable file, or version drift. The vengc verify CLI is a thin wrapper
    // over this.
    [[nodiscard]] int VerifyArchiveCli(const path& archivePath);
}
