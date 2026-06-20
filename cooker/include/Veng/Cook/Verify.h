#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/Types.h>

/// @brief Archive integrity verification via xxh3-128 re-hashing.
///
/// Re-hashes a cooked .vengpack and compares against the stored per-blob hashes and
/// the TOC digest. Cooker-side only — the runtime trusts its packs and never verifies.
/// Reuses the cooker's vendored xxHash and never links libveng.

namespace Veng::Cook
{
    /// @brief Verification verdict for one asset in an archive.
    ///
    /// A zero stored hash counts as a mismatch, since every cooked archive is fully hashed.
    struct VerifiedAsset
    {
        /// @brief The asset's unique identifier.
        AssetId Id;
        /// @brief The asset's type.
        AssetType Type;
        /// @brief True when the re-hashed blob matches the stored per-blob Hash.
        bool Ok = false;
    };

    /// @brief The outcome of verifying one archive.
    struct VerifyReport
    {
        /// @brief Set when the archive could not be opened (unreadable, bad magic, or version mismatch).
        ///
        /// When present, no hashing ran and Assets is empty.
        optional<string> OpenError;

        /// @brief Per-asset verification results.
        vector<VerifiedAsset> Assets;

        /// @brief True when the archive's stored TOC digest matches the value recomputed over TocBytes().
        ///
        /// A zero stored digest counts as a mismatch, like a zero per-blob hash.
        bool DigestOk = false;

        /// @brief Returns true only when the archive opened, every blob matched, and the TOC digest matched.
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

    /// @brief Opens archivePath, re-hashes every blob and the serialized TOC, and compares against stored hashes.
    ///
    /// Returns the full verification report. Never aborts on a corrupt archive — mismatches
    /// are data in the report, not fatal errors.
    /// @param archivePath  Path to the .vengpack to verify.
    [[nodiscard]] VerifyReport VerifyArchive(const path& archivePath);

    /// @brief Runs VerifyArchive and prints a per-asset OK/MISMATCH report to stdout.
    ///
    /// Returns 0 when all blobs and the TOC digest match, 1 on any mismatch, unreadable file,
    /// or version drift.
    /// @param archivePath  Path to the .vengpack to verify.
    [[nodiscard]] int VerifyArchiveCli(const path& archivePath);
}
