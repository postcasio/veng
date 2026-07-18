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
// The SaveSession hook writes to an in-process buffer only — no disk.
//
// The second case pins the exit-status contract on the same machinery: an app that names a failure
// status from OnInitialize stops there (no world bootstrap, no frame), Run returns that status, and
// the shutdown/teardown markers still appear. The default path's status is checked in the first
// case — an app that only calls RequestExit() reports 0.

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "support/BootstrapFixture.h"

#include <Veng/Application.h>
#include <Veng/Net/AccountId.h>
#include <Veng/Net/WorldKey.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/SystemRegistry.h>

using namespace Veng;

namespace
{
    using TestSupport::WriteBootstrapFixture;

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

    // The status the startup-failure app names; any non-zero value, distinct from 1 so a stray
    // generic failure could not pass for it.
    constexpr i32 kStartupFailureStatus = 3;

    // A headless managed-world Application that treats its startup as fatal: OnInitialize names a
    // failure status and returns, standing in for a consumer whose required resource is missing.
    // It logs the world bootstrap and each frame so the test can see neither happened, and shares
    // the shutdown/teardown markers so the teardown is still observable.
    class StartupFailureApp final : public Application
    {
    public:
        StartupFailureApp(ApplicationInfo info, TypeRegistry& types, SystemRegistry& systems,
                          std::vector<std::string>& log)
            : Application(std::move(info), types, systems), m_Log(log)
        {
            m_Probe.Log = &m_Log;
        }

    protected:
        void OnInitialize() override { RequestExit(kStartupFailureStatus); }

        void OnWorldLoaded(WorldInstanceId, Scene&, ResidencyBatch&) override
        {
            m_Log.push_back("world");
        }

        void OnUpdate(f32) override { m_Log.push_back("update"); }

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

    i32 status = -1;
    {
        ShutdownApp app(std::move(info), types, systems, log);
        status = app.Run({});
    }

    // (a) The SaveSession hook fired at all — a deleted SaveAll call drops this marker.
    REQUIRE(std::ranges::find(log, "save") != log.end());
    // (b) The full sequence, in execution order: operations while alive, then destruction.
    REQUIRE(log.size() == 3);
    CHECK(log[0] == "shutdown");
    CHECK(log[1] == "save");
    CHECK(log[2] == "teardown");
    // (c) The default path: an app that only ever calls RequestExit() reports success.
    CHECK(status == 0);
}

TEST_CASE("Application fatal startup failure returns its status and still tears down")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    const path project = WriteBootstrapFixture(types, "startup-failure");

    std::vector<std::string> log;

    ApplicationInfo info;
    info.Name = "veng-application-startup-failure-test";
    info.Headless = true;
    info.ImGui = std::nullopt;
    info.ManagedViewport = ManagedViewportInfo{};
    info.World = GameWorldInfo{.Project = project};
    info.Net = GameNetInfo{};
    info.Net->SaveSession = [&log](Net::AccountId, std::span<const std::byte>)
    { log.push_back("save"); };

    i32 status = -1;
    {
        StartupFailureApp app(std::move(info), types, systems, log);
        status = app.Run({});
    }

    // The status RequestExit(i32) named reaches the caller — what the launcher returns from main.
    CHECK(status == kStartupFailureStatus);
    // Failing in OnInitialize stops initialization: the world never bootstraps and no frame runs.
    CHECK(std::ranges::find(log, "world") == log.end());
    CHECK(std::ranges::find(log, "update") == log.end());
    // Teardown is unaffected — the point of this over terminating the process outright.
    REQUIRE(log.size() == 2);
    CHECK(log[0] == "shutdown");
    CHECK(log[1] == "teardown");
}
