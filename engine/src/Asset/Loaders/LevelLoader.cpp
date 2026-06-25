#include "LevelLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Environment.h>
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

        // Loads a Prefab dependency by id on the active path, returning its handle.
        // A non-resolving id surfaces as a recoverable load failure.
        AssetResult<AssetHandle<Prefab>>
        LoadPrefabDependency(AssetManager& manager, AssetId levelId, AssetId prefabId, bool async)
        {
            if (async)
            {
                AssetHandle<Prefab> handle = manager.Load<Prefab>(prefabId);
                if (!AssetManager::EntryOf(handle))
                {
                    return std::unexpected(
                        AssetLoadError{.Kind = AssetError::MissingDependency,
                                       .Id = prefabId,
                                       .Detail = fmt::format("level {}: prefab {} did not resolve",
                                                             levelId.Value, prefabId.Value)});
                }
                return handle;
            }

            return manager.LoadSync<Prefab>(prefabId);
        }

        // Loads an Environment dependency by id on the active path, returning its handle.
        AssetResult<AssetHandle<Environment>>
        LoadEnvironmentDependency(AssetManager& manager, AssetId levelId, AssetId envId, bool async)
        {
            if (async)
            {
                AssetHandle<Environment> handle = manager.Load<Environment>(envId);
                if (!AssetManager::EntryOf(handle))
                {
                    return std::unexpected(AssetLoadError{
                        .Kind = AssetError::MissingDependency,
                        .Id = envId,
                        .Detail = fmt::format("level {}: environment {} did not resolve",
                                              levelId.Value, envId.Value)});
                }
                return handle;
            }

            return manager.LoadSync<Environment>(envId);
        }
    }

    AssetResult<Detail::LoadJob>
    LevelLoader::Load(AssetManager& manager, Renderer::Context& /*context*/, TaskSystem& /*tasks*/,
                      TypeRegistry& types, AssetId id, std::span<const u8> cooked, bool async) const
    {
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

        // ── 4. Resolve the world prefab and the game-mode player prefab ──────
        const AssetResult<AssetHandle<Prefab>> world =
            LoadPrefabDependency(manager, id, AssetId{header.WorldPrefabId}, async);
        if (!world)
        {
            return std::unexpected(world.error());
        }

        // The decoded handle carries only the raw id; rebind it to a live, resident handle so
        // the Session entity the level seeds reports the player prefab as loaded.
        const AssetId playerId = gameMode.PlayerPrefab.Id();
        if (playerId.IsValid())
        {
            const AssetResult<AssetHandle<Prefab>> player =
                LoadPrefabDependency(manager, id, playerId, async);
            if (!player)
            {
                return std::unexpected(player.error());
            }
            gameMode.PlayerPrefab = *player;
        }

        // The render settings carry an environment map (id-only after ReadFields); rebind it to a
        // resident handle so the app reads a loaded environment off the level.
        const AssetId envId = render.Environment.Id();
        if (envId.IsValid())
        {
            const AssetResult<AssetHandle<Environment>> environment =
                LoadEnvironmentDependency(manager, id, envId, async);
            if (!environment)
            {
                return std::unexpected(environment.error());
            }
            render.Environment = *environment;
        }

        // ── 5. Collect dependency cache entries (kept resident for the level) ─
        vector<Ref<Detail::AssetCacheEntry>> dependencies;
        dependencies.push_back(AssetManager::EntryOf(*world));
        if (playerId.IsValid())
        {
            dependencies.push_back(AssetManager::EntryOf(gameMode.PlayerPrefab));
        }
        if (envId.IsValid())
        {
            dependencies.push_back(AssetManager::EntryOf(render.Environment));
        }

        // ── 6. Construct the Level ───────────────────────────────────────────
        const Ref<Level> level =
            Level::Create(*world, std::move(systems), std::move(gameMode), render);

        return Detail::LoadJob{
            .Resource = Detail::RefAny(level),
            .Dependencies = std::move(dependencies),
            .Finalize = []() -> VoidResult { return {}; },
        };
    }
}
