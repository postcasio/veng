#include <Veng/Cook/CookTiming.h>

#include <algorithm>
#include <map>
#include <span>

#include <fmt/format.h>

#include <Veng/Asset/AtomicFile.h>

namespace Veng::Cook
{
    namespace
    {
        /// One importer's aggregated cost across the cook.
        struct ImporterRollup
        {
            usize Count = 0;
            usize Hits = 0;
            f64 Seconds = 0.0;
        };

        // Percentage of `total`, or zero when there is nothing to divide by — a report that
        // divides by an unmeasured total prints nan and reads as a broken instrument.
        f64 Percent(f64 part, f64 total)
        {
            return total > 0.0 ? (part / total) * 100.0 : 0.0;
        }

        void AppendPhase(string& out, const char* label, f64 seconds, f64 total)
        {
            out += fmt::format("    {:<16}{:>10.3f} s{:>8.1f} %\n", label, seconds,
                               Percent(seconds, total));
        }
    }

    f64 CookTiming::AssetSeconds() const
    {
        f64 sum = 0.0;
        for (const CookAssetTiming& asset : Assets)
        {
            sum += asset.TotalSeconds();
        }
        return sum;
    }

    f64 CookTiming::NamedPhaseSeconds() const
    {
        return ModuleLoadSeconds + ProjectParseSeconds + ManifestParseSeconds + TocDigestSeconds +
               ArchiveWriteSeconds + CacheStoreSeconds + DepfileSeconds;
    }

    usize CookTiming::CacheHits() const
    {
        usize hits = 0;
        for (const CookAssetTiming& asset : Assets)
        {
            if (asset.CacheHit)
            {
                ++hits;
            }
        }
        return hits;
    }

    string FormatCookTimingSummary(const CookTiming& timing, usize topCount)
    {
        const f64 total = timing.TotalSeconds;
        const f64 assetSeconds = timing.AssetSeconds();
        const f64 remainder = total - assetSeconds;
        const usize hits = timing.CacheHits();

        f64 importSeconds = 0.0;
        f64 lookupSeconds = 0.0;
        f64 storeSeconds = 0.0;
        f64 waitSeconds = 0.0;
        for (const CookAssetTiming& asset : timing.Assets)
        {
            importSeconds += asset.ImportSeconds;
            lookupSeconds += asset.CacheLookupSeconds;
            storeSeconds += asset.StoreSeconds;
            waitSeconds += asset.SerializedWaitSeconds;
        }

        string out =
            fmt::format("cook timing — {} assets, {} cache hits / {} fresh, {} jobs\n",
                        timing.Assets.size(), hits, timing.Assets.size() - hits, timing.Jobs);
        out += fmt::format("  total             {:>10.3f} s\n", total);
        out += fmt::format("  assets            {:>10.3f} s{:>8.1f} %{}\n", assetSeconds,
                           Percent(assetSeconds, total),
                           timing.Jobs > 1 ? "  (summed across workers)" : "");
        AppendPhase(out, "import", importSeconds, total);
        AppendPhase(out, "cache lookup", lookupSeconds, total);
        AppendPhase(out, "blob store", storeSeconds, total);
        AppendPhase(out, "serialized wait", waitSeconds, total);
        // Above one job the assets ran concurrently, so total minus their sum is not a remainder —
        // it can even be negative. The named phases are all measured on the driving thread and stay
        // meaningful either way, so they are what is reported.
        if (timing.Jobs > 1)
        {
            // Time blocked on the serialization lock is elapsed but not work, so the occupancy
            // figure excludes it; counting it would report cores that were only ever waiting.
            out += fmt::format("  workers busy      {:>10.3f}\n",
                               Percent(assetSeconds - waitSeconds, total) / 100.0);
            out += "  driver phases\n";
        }
        else
        {
            out += fmt::format("  remainder         {:>10.3f} s{:>8.1f} %\n", remainder,
                               Percent(remainder, total));
        }
        AppendPhase(out, "module load", timing.ModuleLoadSeconds, total);
        AppendPhase(out, "project parse", timing.ProjectParseSeconds, total);
        AppendPhase(out, "manifest parse", timing.ManifestParseSeconds, total);
        AppendPhase(out, "toc + digest", timing.TocDigestSeconds, total);
        AppendPhase(out, "archive write", timing.ArchiveWriteSeconds, total);
        AppendPhase(out, "cache store", timing.CacheStoreSeconds, total);
        AppendPhase(out, "depfile", timing.DepfileSeconds, total);
        if (timing.Jobs <= 1)
        {
            AppendPhase(out, "unattributed", remainder - timing.NamedPhaseSeconds(), total);
        }

        // The costliest assets, which is what decides whether a cook's cost is a long tail or one
        // dominant asset.
        vector<const CookAssetTiming*> ranked;
        ranked.reserve(timing.Assets.size());
        for (const CookAssetTiming& asset : timing.Assets)
        {
            ranked.push_back(&asset);
        }
        std::ranges::sort(ranked, [](const CookAssetTiming* a, const CookAssetTiming* b)
                          { return a->TotalSeconds() > b->TotalSeconds(); });

        const usize shown = std::min(topCount, ranked.size());
        out += fmt::format("\n  top {} assets\n", shown);
        for (usize i = 0; i < shown; ++i)
        {
            const CookAssetTiming& asset = *ranked[i];
            out += fmt::format("    {:>10.3f} s{:>8.1f} %  {:<20} 0x{:016X}  {}\n",
                               asset.TotalSeconds(), Percent(asset.TotalSeconds(), total),
                               asset.Type, asset.Id.Value, asset.CacheHit ? "hit" : "fresh");
        }

        std::map<string, ImporterRollup> byImporter;
        for (const CookAssetTiming& asset : timing.Assets)
        {
            ImporterRollup& rollup = byImporter[asset.Type];
            ++rollup.Count;
            rollup.Hits += asset.CacheHit ? 1 : 0;
            rollup.Seconds += asset.TotalSeconds();
        }

        vector<std::pair<string, ImporterRollup>> rollups(byImporter.begin(), byImporter.end());
        std::ranges::sort(rollups, [](const auto& a, const auto& b)
                          { return a.second.Seconds > b.second.Seconds; });

        out += fmt::format("\n  per importer ({})\n", rollups.size());
        out += "    importer              count    hits       total    share        mean\n";
        for (const auto& [name, rollup] : rollups)
        {
            out += fmt::format("    {:<20}{:>7}{:>8}{:>10.3f} s{:>8.1f} %{:>10.3f} s\n", name,
                               rollup.Count, rollup.Hits, rollup.Seconds,
                               Percent(rollup.Seconds, total),
                               rollup.Seconds / static_cast<f64>(rollup.Count));
        }

        return out;
    }

    VoidResult WriteCookTimingTable(const path& file, const CookTiming& timing)
    {
        string csv =
            "id,type,cache_hit,cache_lookup_s,serialized_wait_s,import_s,store_s,total_s\n";
        for (const CookAssetTiming& asset : timing.Assets)
        {
            csv += fmt::format("0x{:016X},{},{},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f}\n",
                               asset.Id.Value, asset.Type, asset.CacheHit ? 1 : 0,
                               asset.CacheLookupSeconds, asset.SerializedWaitSeconds,
                               asset.ImportSeconds, asset.StoreSeconds, asset.TotalSeconds());
        }

        const VoidResult written =
            WriteFileAtomic(file, std::span(reinterpret_cast<const u8*>(csv.data()), csv.size()));
        if (!written)
        {
            return std::unexpected(fmt::format("timing '{}': {}", file.string(), written.error()));
        }
        return {};
    }
}
