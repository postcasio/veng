// Application shutdown ordering: proves Run() runs its operations in order — the app's OnShutdown()
// then the engine's session SaveAll() — while every service is still alive, and that resource
// teardown (member destruction) follows only after Run() returns.
//
// The -L validation gate cannot see this: deleting the SaveAll() call yields a clean, leak-free,
// validation-green shutdown that silently drops session state. So this test registers a test-owned
// SaveSession hook and an OnShutdown override, both appending to a shared log, seeds one dirty
// session record, and on quit asserts (a) the hook fired at all — catching a deleted SaveAll — and
// (b) that the log order is OnShutdown -> SaveSession -> member teardown — catching a re-homing that
// inverts it.
//
// It rides the gpu band because Application owns a Context; the assertion itself touches no device.
// The SaveSession hook writes to an in-process buffer only — no disk, no game.

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "support/TempPath.h"

#include <fmt/format.h>

#include <Veng/Application.h>
#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/CookedProject.h>
#include <Veng/Net/AccountId.h>
#include <Veng/Net/WorldKey.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/SystemRegistry.h>

using namespace Veng;

namespace
{
    template <class T>
    void PushPod(vector<u8>& out, const T& value)
    {
        const auto* p = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), p, p + sizeof(value));
    }

    // Writes a minimal cooked world (an empty world prefab + a level referencing it, no systems) into
    // a .vengpack, plus a .vengproj naming the pack and the startup level. Every path is absolute, so
    // the bootstrap's ExecutableDirectory()-relative resolve (ExecutableDirectory() / absolute ==
    // absolute) lands on these temp files. Returns the project path to feed ApplicationInfo::World.
    path WriteBootstrapFixture(const TypeRegistry& types, const char* tag)
    {
        const AssetId prefabId{0x51D0000000000001ULL};
        const AssetId levelId{0x51D0000000000002ULL};

        vector<u8> prefab;
        PushPod(prefab, CookedPrefabHeader{.Version = CookedPrefabVersion});

        const GameModeConfig gameMode;
        vector<u8> gameModeRecord;
        WriteFields(gameModeRecord, &gameMode, types.Info(TypeIdOf<GameModeConfig>()), types);
        const LevelRenderSettings renderSettings;
        vector<u8> renderRecord;
        WriteFields(renderRecord, &renderSettings, types.Info(TypeIdOf<LevelRenderSettings>()),
                    types);

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
        writer.Add(prefabId, AssetType::Prefab, prefab);
        writer.Add(levelId, AssetType::Level, level);

        const path packPath =
            TestSupport::TempDir() / fmt::format("veng_shutdown_{}.vengpack", tag);
        REQUIRE(writer.Write(packPath).has_value());

        CookedProject project;
        project.StartupLevel = levelId;
        project.PackMountNames = {packPath.string()};
        const path projectPath =
            TestSupport::TempDir() / fmt::format("veng_shutdown_{}.vengproj", tag);
        REQUIRE(WriteCookedProject(projectPath, project).has_value());
        return projectPath;
    }

    // A headless managed-world Application whose OnShutdown, SaveSession hook, and a member probe each
    // append a marker to a shared log, so the log records the shutdown sequence in execution order.
    class ShutdownApp final : public Application
    {
    public:
        ShutdownApp(ApplicationInfo info, TypeRegistry& types, SystemRegistry& systems,
                    std::vector<std::string>& log)
            : Application(std::move(info), types, systems), m_Log(log)
        {
            // The probe destructs in ~ShutdownApp (before ~Application), i.e. after Run() returns —
            // after OnShutdown and SaveAll — so it marks the resource-teardown phase.
            m_Probe.Log = &m_Log;
        }

    protected:
        void OnUpdate(f32) override
        {
            if (!m_Seeded)
            {
                // Present-travel to the managed world (registered under DefaultWorldKey) records a
                // dirty gameplay session for the local account — no WorldFactory needed, the key
                // resolves to the pre-registered bucket. That is the one record SaveAll must flush.
                const VoidResult travelled =
                    Travel(TravelInfo{.Key = Net::DefaultWorldKey, .Present = true});
                REQUIRE(travelled.has_value());
                m_Seeded = true;
            }
            RequestExit();
        }

        void OnShutdown() override { m_Log.push_back("shutdown"); }

    private:
        struct TeardownProbe
        {
            std::vector<std::string>* Log = nullptr;
            ~TeardownProbe()
            {
                if (Log != nullptr)
                {
                    Log->push_back("teardown");
                }
            }
        };

        TeardownProbe m_Probe;
        std::vector<std::string>& m_Log;
        bool m_Seeded = false;
    };
}

TEST_CASE("Application shutdown runs OnShutdown, then SaveAll, then member teardown")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    const path project = WriteBootstrapFixture(types, "ordering");

    std::vector<std::string> log;

    ApplicationInfo info;
    info.Name = "veng-application-shutdown-test";
    info.Headless = true;
    info.ImGui = std::nullopt;
    info.ManagedViewport = ManagedViewportInfo{};
    info.World = GameWorldInfo{.Project = project};
    info.Net = GameNetInfo{};
    // The durability write half, into a test buffer only (no disk): the marker SaveAll must emit.
    info.Net->SaveSession = [&log](Net::AccountId, std::span<const std::byte>)
    { log.push_back("save"); };

    {
        ShutdownApp app(std::move(info), types, systems, log);
        app.Run({});
    }

    // (a) The SaveSession hook fired at all — a deleted SaveAll call drops this marker.
    REQUIRE(std::ranges::find(log, "save") != log.end());
    // (b) The full sequence, in execution order: operations while alive, then destruction.
    REQUIRE(log.size() == 3);
    CHECK(log[0] == "shutdown");
    CHECK(log[1] == "save");
    CHECK(log[2] == "teardown");
}
