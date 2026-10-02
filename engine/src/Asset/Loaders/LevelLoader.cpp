#include "LevelLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }
    }

    AssetResult<Detail::ParsedAsset> LevelLoader::Parse(const AssetParseContext& context,
                                                        const AssetId id,
                                                        const std::span<const u8> cooked) const
    {
        const TypeRegistry& types = context.Types;
        // ── 1. CookedLevelHeader ─────────────────────────────────────────────
        if (cooked.size() < sizeof(CookedLevelHeader))
        {
            return std::unexpected(
                Corrupt(id, "level: cooked blob smaller than CookedLevelHeader"));
        }

        CookedLevelHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        // A stale/foreign blob is a recoverable load failure, not a crash.
        if (header.Version != CookedLevelVersion)
        {
            return std::unexpected(
                Corrupt(id, fmt::format("level: blob version {} does not match expected version {}",
                                        header.Version, CookedLevelVersion)));
        }

        const usize systemBytes = static_cast<usize>(header.SystemCount) * sizeof(u64);
        usize cursor = sizeof(CookedLevelHeader);
        if (cooked.size() <
            cursor + systemBytes + header.GameModeRecordBytes + header.RenderRecordBytes)
        {
            return std::unexpected(Corrupt(id, "level: cooked blob truncated"));
        }

        // ── 2. The ordered system-id set ─────────────────────────────────────
        vector<SystemId> systems(header.SystemCount);
        if (systemBytes > 0)
        {
            std::memcpy(systems.data(), cooked.data() + cursor, systemBytes);
        }
        cursor += systemBytes;

        // ── 3. The two tolerant reflection records ───────────────────────────
        const std::span<const u8> gameModeRecord =
            cooked.subspan(cursor, header.GameModeRecordBytes);
        cursor += header.GameModeRecordBytes;
        const std::span<const u8> renderRecord = cooked.subspan(cursor, header.RenderRecordBytes);

        GameModeConfig gameMode;
        const VoidResult gmRead =
            ReadFields(gameModeRecord, &gameMode, types.Info(TypeIdOf<GameModeConfig>()), types);
        if (!gmRead)
        {
            return std::unexpected(Corrupt(id, gmRead.error()));
        }

        LevelRenderSettings render;
        const VoidResult renderRead =
            ReadFields(renderRecord, &render, types.Info(TypeIdOf<LevelRenderSettings>()), types);
        if (!renderRead)
        {
            return std::unexpected(Corrupt(id, renderRead.error()));
        }

        // ── 4. Name the world prefab and the game-mode player prefab ─────────
        // Both are dependencies the level keeps resident. The environment map is not a level field:
        // it rides an EnvironmentSky source on the world prefab's Sky component and resolves
        // through the prefab's own dependency walk like any embedded handle.
        const AssetId worldId{header.WorldPrefabId};
        const AssetId playerId = gameMode.PlayerPrefab.Id();
        Detail::ParsedAsset parsed;
        parsed.Dependencies.push_back({.Type = AssetTypes::Prefab, .Id = worldId});
        if (playerId.IsValid())
        {
            parsed.Dependencies.push_back({.Type = AssetTypes::Prefab, .Id = playerId});
        }

        // ── 5. Construct the Level once the prefabs are named in the cache ───
        parsed.Complete = [systems = std::move(systems), gameMode = std::move(gameMode),
                           render](AssetManager&,
                                   std::span<const Ref<Detail::AssetCacheEntry>> resolved) mutable
            -> AssetResult<Detail::LoadJob>
        {
            // The decoded handle carries only the raw id; rebind it to the live entry so the
            // settings entity the level seeds reports the player prefab as loaded.
            const AssetHandle<Prefab> world = AssetManager::HandleOf<Prefab>(resolved[0]);
            if (resolved.size() > 1)
            {
                gameMode.PlayerPrefab = AssetManager::HandleOf<Prefab>(resolved[1]);
            }
            const Ref<Level> level =
                Level::Create(world, std::move(systems), std::move(gameMode), render);
            return Detail::LoadJob{
                .Resource = Detail::RefAny(level),
                .Dependencies =
                    vector<Ref<Detail::AssetCacheEntry>>(resolved.begin(), resolved.end()),
                .Finalize = []() -> VoidResult { return {}; },
            };
        };
        return parsed;
    }
}
