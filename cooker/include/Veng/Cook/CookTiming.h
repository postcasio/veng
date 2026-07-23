#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/Path.h>
#include <Veng/Cook/Types.h>

#include <chrono>

namespace Veng::Cook
{
    /// @brief Monotonic wall-clock stopwatch, started on construction.
    ///
    /// `steady_clock` rather than `system_clock`: a wall reading that a clock adjustment can move
    /// backwards is useless for a duration. The cook records a few hundred events, so the sampling
    /// cost is irrelevant beside the work being measured.
    class CookStopwatch
    {
    public:
        /// @brief Starts the stopwatch.
        CookStopwatch() : m_Start(std::chrono::steady_clock::now()) {}

        /// @brief Returns the seconds elapsed since construction.
        [[nodiscard]] f64 Elapsed() const
        {
            const std::chrono::duration<f64> elapsed = std::chrono::steady_clock::now() - m_Start;
            return elapsed.count();
        }

    private:
        /// @brief The instant the stopwatch was constructed.
        std::chrono::steady_clock::time_point m_Start;
    };

    /// @brief Wall-clock cost of one pack entry, split into the phases that behave differently.
    ///
    /// The three phases are kept apart deliberately: a cache hit and a fresh cook are different
    /// events, and averaging the importer's cost together with the cache lookup and the
    /// compress-and-hash step hides whichever of them is the real cost.
    struct CookAssetTiming
    {
        /// @brief The asset id the pack manifest entry declares.
        AssetId Id;

        /// @brief The registered name of the entry's asset type.
        ///
        /// The importer table is keyed by asset type — one importer per type — so this also names
        /// the importer the entry was dispatched to, and rolling up by it is a per-importer roll-up.
        string Type;

        /// @brief True when the entry was served from the cook cache rather than cooked.
        bool CacheHit = false;

        /// @brief Seconds spent computing the entry's cache key and validating a candidate entry.
        f64 CacheLookupSeconds = 0.0;

        /// @brief Seconds spent inside the importer. Zero on a cache hit.
        ///
        /// Excludes @ref SerializedWaitSeconds, so this is the importer's own work whether it ran
        /// concurrently or waited its turn.
        f64 ImportSeconds = 0.0;

        /// @brief Seconds spent waiting for the cook's serialization lock before the importer ran.
        ///
        /// Nonzero only for an importer that did not declare itself reentrant, and only when
        /// another such importer held the lock. Summed across the pack, this is what parallelizing
        /// the remaining importers could still buy.
        f64 SerializedWaitSeconds = 0.0;

        /// @brief Seconds spent compressing and hashing the entry's blobs. Zero on a cache hit.
        f64 StoreSeconds = 0.0;

        /// @brief Total wall seconds attributed to this asset.
        [[nodiscard]] f64 TotalSeconds() const
        {
            return CacheLookupSeconds + SerializedWaitSeconds + ImportSeconds + StoreSeconds;
        }
    };

    /// @brief One `vengc` invocation's timing record: every asset, plus the named whole-cook phases.
    ///
    /// The phases exist so the report's remainder is *stated* rather than left as a subtraction the
    /// reader has to interpret: a per-asset breakdown that quietly omits a serial share would point
    /// an optimization at the wrong place. Whatever the named phases still fail to account for is
    /// reported as an explicit unattributed line.
    ///
    /// A `cook-project` invocation accumulates every pack into one record.
    struct CookTiming
    {
        /// @brief One entry per pack asset, in cook order.
        vector<CookAssetTiming> Assets;

        /// @brief Seconds spent loading the runtime and cook modules and reflecting their types.
        f64 ModuleLoadSeconds = 0.0;

        /// @brief Seconds spent parsing the project file and its build configurations.
        f64 ProjectParseSeconds = 0.0;

        /// @brief Seconds spent parsing pack manifests, including the reference packs.
        f64 ManifestParseSeconds = 0.0;

        /// @brief Seconds spent laying out the archive TOC and hashing it into the pack digest.
        f64 TocDigestSeconds = 0.0;

        /// @brief Seconds spent assembling the archive and writing it, including cached-blob reads.
        f64 ArchiveWriteSeconds = 0.0;

        /// @brief Seconds spent on cook-cache bookkeeping: hashing an entry's inputs, persisting it.
        f64 CacheStoreSeconds = 0.0;

        /// @brief Seconds spent writing the depfile.
        f64 DepfileSeconds = 0.0;

        /// @brief Seconds the whole invocation took, measured from the tool's entry point.
        f64 TotalSeconds = 0.0;

        /// @brief The concurrency budget the cook ran under: threads shared by the asset pool and
        /// any importer-internal parallelism.
        ///
        /// Above one, the per-asset seconds below are a **sum across workers**, not a share of the
        /// wall clock, and their total may exceed @ref TotalSeconds. The report says so rather than
        /// printing a negative remainder.
        u32 Jobs = 1;

        /// @brief Sum of every asset's total, the share of the cook the per-asset table accounts for.
        [[nodiscard]] f64 AssetSeconds() const;

        /// @brief Sum of the named non-asset phases.
        [[nodiscard]] f64 NamedPhaseSeconds() const;

        /// @brief Number of assets served from the cook cache.
        [[nodiscard]] usize CacheHits() const;
    };

    /// @brief Renders the operator-facing summary: the accounting, the costliest assets, the roll-up.
    ///
    /// The report an operator reads. It states the total, the per-asset sum, and the remainder
    /// broken into its named phases; then the `topCount` costliest assets; then a per-importer
    /// roll-up with counts and totals.
    /// @param timing    The collected record.
    /// @param topCount  How many of the costliest assets to list.
    /// @return The formatted, newline-terminated report.
    [[nodiscard]] string FormatCookTimingSummary(const CookTiming& timing, usize topCount = 10);

    /// @brief Writes the full per-asset table as CSV for a tool to consume.
    ///
    /// One header row, then one row per asset:
    /// `id,type,cache_hit,cache_lookup_s,import_s,store_s,total_s`. The summary carries the
    /// whole-cook phases; this file is the per-asset table alone.
    /// Errors are located: `"timing '<path>': <reason>"`.
    /// @param file    Destination CSV path.
    /// @param timing  The collected record.
    [[nodiscard]] VoidResult WriteCookTimingTable(const path& file, const CookTiming& timing);
}
