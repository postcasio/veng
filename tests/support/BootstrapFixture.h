#pragma once

// The cooked-world fixture a test that boots a real managed-world Application needs: a minimal
// prefab + level + project synthesized on disk, so ApplicationInfo::World has something to
// bootstrap from with no cooked asset dependency of its own.

#include <cstdint>
#include <span>
#include <vector>

#include <doctest/doctest.h>

#include <fmt/format.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/CookedProject.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Veng.h>

#include "support/TempPath.h"

namespace Veng::TestSupport
{
    /// @brief Appends a trivially-copyable value's bytes to a byte buffer.
    /// @param out    The buffer appended to.
    /// @param value  The value whose object representation is appended.
    template <class T>
    void PushPod(vector<u8>& out, const T& value)
    {
        const auto* p = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), p, p + sizeof(value));
    }

    /// @brief One extra cooked asset a fixture's pack carries beside the world prefab and level.
    ///
    /// For a test whose Application names a boot asset by id — a settings schema, a locale index —
    /// so the blob the engine's boot LoadSync resolves is in the pack it mounts.
    struct BootstrapAsset
    {
        /// @brief The asset's id, as the ApplicationInfo field naming it must spell.
        AssetId Id;
        /// @brief The asset's cooked type tag, selecting the loader that decodes the blob.
        AssetTypeId Type;
        /// @brief The cooked blob, byte-for-byte as that loader reads it.
        vector<u8> Blob;
    };

    /// @brief Writes a minimal cooked world (empty world prefab + level, no systems) plus its project.
    ///
    /// The pack holds an empty world prefab and a level referencing it; the `.vengproj` names the pack
    /// and that level as the startup level. Every path is absolute, so the bootstrap's
    /// ExecutableDirectory()-relative resolve (ExecutableDirectory() / absolute == absolute) lands on
    /// these temp files.
    /// @param types  The registry holding the GameModeConfig / RenderLook schemas.
    /// @param tag    A per-test suffix keeping concurrent fixtures in one process distinct.
    /// @param extra  Further cooked assets to place in the same pack; empty for a bare world.
    /// @return The project path to feed ApplicationInfo::World.
    inline path WriteBootstrapFixture(const TypeRegistry& types, const char* tag,
                                      std::span<const BootstrapAsset> extra = {})
    {
        const AssetId prefabId{0x51D0000000000001ULL};
        const AssetId levelId{0x51D0000000000002ULL};

        vector<u8> prefab;
        PushPod(prefab, CookedPrefabHeader{.Version = CookedPrefabVersion});

        const GameModeConfig gameMode;
        vector<u8> gameModeRecord;
        WriteFields(gameModeRecord, &gameMode, types.Info(TypeIdOf<GameModeConfig>()), types);
        const RenderLook renderSettings;
        vector<u8> renderRecord;
        WriteFields(renderRecord, &renderSettings, types.Info(TypeIdOf<RenderLook>()), types);

        vector<u8> level;
        PushPod(level, CookedLevelHeader{
                           .Version = CookedLevelVersion,
                           .WorldPrefabId = prefabId.Value,
                           .SystemCount = 0,
                           .GameModeRecordBytes = static_cast<u32>(gameModeRecord.size()),
                           .RenderRecordBytes = static_cast<u32>(renderRecord.size()),
                       });
        level.insert(level.end(), gameModeRecord.begin(), gameModeRecord.end());
        level.insert(level.end(), renderRecord.begin(), renderRecord.end());

        ArchiveWriter writer;
        writer.Add(prefabId, AssetTypes::Prefab, prefab);
        writer.Add(levelId, AssetTypes::Level, level);
        for (const BootstrapAsset& asset : extra)
        {
            writer.Add(asset.Id, asset.Type, asset.Blob);
        }

        const path packPath = TempDir() / fmt::format("veng_bootstrap_{}.vengpack", tag);
        REQUIRE(writer.Write(packPath).has_value());

        CookedProject project;
        project.StartupLevel = levelId;
        project.PackMountNames = {packPath.string()};
        const path projectPath = TempDir() / fmt::format("veng_bootstrap_{}.vengproj", tag);
        REQUIRE(WriteCookedProject(projectPath, project).has_value());
        return projectPath;
    }
}
