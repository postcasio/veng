// The boot-restore gate: GameWorldInfo::RestoreLocalSessionOnBoot decides whether the bootstrap
// calls RestoreLocalSession once world #0 is bound. Cleared, the engine owes the consumer exactly
// one guarantee — it performs no restore at all, so world #0 (the startup level) stays presented
// and nothing of the account's record is read until the consumer asks. A consumer whose record
// lives in a store it opens later relies on that silence.
//
// Only a real Application can see it: the branch is in BootstrapWorld, so a unit case modelling the
// orchestration over a bare SessionRegistry + WorldDirectory never reaches it, and a smoke run only
// proves the boot does not crash — not that no rebind occurred. So this drives a headless
// managed-world Application through Run() and reads the managed viewport's binding directly.
//
// The gameplay key must genuinely resolve, or the test proves nothing: a *denied* resolve also
// leaves the current world presented, so a non-resolving key cannot tell "the gate suppressed the
// restore" from "the restore ran and failed". A WorldFactory therefore opens a real empty, ticking
// world per key through the app's own runner — distinct from world #0 and genuinely presentable —
// and the LoadSession / TransformOnReattach hooks count how far the restore path got.
//
// It rides the gpu band because Application owns a Context; the assertions touch no device. Every
// hook reads and writes in-process buffers only — no disk beyond the synthesized cooked fixture.

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include <doctest/doctest.h>

#include "support/BootstrapFixture.h"

#include <Veng/Application.h>
#include <Veng/ManagedViewports.h>
#include <Veng/Net/AccountId.h>
#include <Veng/Net/Blob.h>
#include <Veng/Net/Session.h>
#include <Veng/Net/WorldKey.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/WorldDirectory.h>
#include <Veng/WorldRunner.h>

using namespace Veng;

namespace
{
    using TestSupport::WriteBootstrapFixture;

    // Two gameplay keys neither of which is Net::DefaultWorldKey, so each resolves through the
    // factory to its own world rather than back to the pre-registered world #0 bucket.
    constexpr Net::WorldKey FirstGameplayKey{.Lo = 0x9A11, .Hi = 0x9A12};
    constexpr Net::WorldKey SecondGameplayKey{.Lo = 0x9A21, .Hi = 0x9A22};
    constexpr Net::AccountId TestAccount{.Lo = 0xB007};

    // Frames to drive while waiting for a present-on-ready rebind to apply. A factory-opened world
    // becomes presentable once its clock has run a fixed sim step, which is a wall-clock wait at the
    // sim rate; this budget is far past that and bounds a regression instead of hanging.
    constexpr u32 MaxWaitFrames = 4000;

    // Everything the restore path writes as it runs, so a case can tell how far it got: the store the
    // record is loaded from, the hook call counts, the pose the reattach transform saw, and the world
    // the factory opened per key.
    struct RestoreProbe
    {
        vector<std::byte> Store;
        u32 Loads = 0;
        u32 Reattaches = 0;
        u32 FactoryOpens = 0;
        Net::Blob ReattachedPose;
        std::unordered_map<Net::WorldKey, WorldInstanceId> WorldOfKey;
    };

    // A headless managed-world Application that snapshots the post-bootstrap presentation state on
    // its first frame and then runs a per-case script, so each case drives the on-demand calls itself.
    class BootRestoreApp final : public Application
    {
    public:
        BootRestoreApp(ApplicationInfo info, TypeRegistry& types, SystemRegistry& systems,
                       RestoreProbe& probe, function<void(BootRestoreApp&, u32)> script)
            : Application(std::move(info), types, systems), m_Probe(probe),
              m_Script(std::move(script))
        {
        }

        // The world factory, reached through the slot the test installs into GameNetInfo::WorldFactory.
        // Opens an empty world with a simulation running no systems: it ticks, so it readies for a
        // present-on-ready rebind, and it is a different world from #0, so the rebind is observable.
        optional<ServerWorldResolution> OpenGameplayWorld(const Net::WorldKey& key)
        {
            ++m_Probe.FactoryOpens;
            const WorldInstanceId world = GetWorldRunner().OpenWorld(WorldOpenInfo{
                .Systems = vector<SystemId>{},
                .MakeStartContext =
                    [this]
                {
                    return SystemContext{
                        .Assets = GetAssetManager(), .Input = GetInput(), .Tasks = GetTaskSystem()};
                },
            });
            m_Probe.WorldOfKey.insert_or_assign(key, world);
            return ServerWorldResolution{
                .WorldId = world, .World = &GetWorldRunner().ResolveWorld(world)->GetScene()};
        }

        /// @brief The world managed viewport 0 presents right now (its applied binding).
        [[nodiscard]] WorldInstanceId PresentedWorld() const
        {
            return GetManagedViewports().GetViewportWorld(0);
        }

        /// @brief The destination of viewport 0's in-flight rebind, or nullopt when none is pending.
        [[nodiscard]] optional<WorldInstanceId> PendingWorld() const
        {
            return GetManagedViewports().GetPendingViewportWorld(0);
        }

        /// @brief Ends the run; the public reach onto the protected RequestExit, for the case scripts.
        void Quit() { RequestExit(); }

        // The presentation state as the bootstrap left it, read before this run's first script step.
        WorldInstanceId BootPresented;
        optional<WorldInstanceId> BootPending;
        u32 BootLoads = 0;
        u32 BootFactoryOpens = 0;

    protected:
        void OnUpdate(f32) override
        {
            ++m_Frame;
            if (m_Frame == 1)
            {
                BootPresented = PresentedWorld();
                BootPending = PendingWorld();
                BootLoads = m_Probe.Loads;
                BootFactoryOpens = m_Probe.FactoryOpens;
            }
            m_Script(*this, m_Frame);
            if (m_Frame >= MaxWaitFrames)
            {
                RequestExit();
            }
        }

    private:
        RestoreProbe& m_Probe;
        function<void(BootRestoreApp&, u32)> m_Script;
        u32 m_Frame = 0;
    };

    // The ApplicationInfo every case shares, minus the gate: headless, no ImGui, one managed viewport,
    // a fixed local account (so RestoreLocalSession has one to resolve against), and the store hooks.
    ApplicationInfo MakeInfo(const path& project, RestoreProbe& probe, BootRestoreApp** slot)
    {
        ApplicationInfo info;
        info.Name = "veng-application-boot-restore-test";
        info.Headless = true;
        info.ImGui = std::nullopt;
        info.ManagedViewport = ManagedViewportInfo{};
        info.World = GameWorldInfo{.Project = project};
        info.Net = GameNetInfo{};
        info.Net->Identity = [] { return TestAccount; };
        info.Net->LoadSession = [&probe](Net::AccountId) -> optional<vector<std::byte>>
        {
            ++probe.Loads;
            return probe.Store;
        };
        // The reattach transform is the record's own witness: it runs only when the restore actually
        // consults the record, and it sees the pose that restore will deliver.
        info.Net->TransformOnReattach = [&probe](Net::SessionRecord record)
        {
            ++probe.Reattaches;
            probe.ReattachedPose = record.Gameplay.Pose;
            return record;
        };
        info.Net->WorldFactory = [slot](const Net::JoinRequestInfo&, const Net::WorldKey& key,
                                        const Net::Blob&) -> optional<ServerWorldResolution>
        { return (*slot)->OpenGameplayWorld(key); };
        return info;
    }

    // A record naming one gameplay world and carrying an arrival pose, as a consumer's store holds it.
    vector<std::byte> EncodeRecord(const TypeRegistry& types, const Net::WorldKey& key,
                                   const u8 poseByte)
    {
        const Net::SessionRecord record{
            .Account = TestAccount,
            .Gameplay = Net::SessionGameplayEntry{
                .Key = key, .Pose = Net::Blob{.Type = TypeIdOf<Transform>(), .Bytes = {poseByte}}}};
        return EncodeSessionRecord(record, types);
    }
}

TEST_CASE("Boot restore opted out performs no rebind, and the on-demand restore does it instead")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    const path project = WriteBootstrapFixture(types, "boot_restore_off");

    RestoreProbe probe;
    probe.Store = EncodeRecord(types, FirstGameplayKey, 0x11);

    BootRestoreApp* slot = nullptr;
    ApplicationInfo info = MakeInfo(project, probe, &slot);
    info.World->RestoreLocalSessionOnBoot = false;

    // What the on-demand restore produced, read out of the run for the assertions below.
    optional<WorldInstanceId> restorePending;
    u32 restoreLoads = 0;
    WorldInstanceId settledWorld;
    WorldInstanceId secondSettledWorld;
    u32 secondRestoreLoads = 0;

    const auto script = [&](BootRestoreApp& app, const u32 frame)
    {
        if (frame == 1)
        {
            // The gate is cleared, so the bootstrap owes silence: nothing may have been requested of
            // the store or the factory yet. Asserted here, before the first on-demand call.
            app.RestoreLocalSession();
            restorePending = app.PendingWorld();
            restoreLoads = probe.Loads;
            return;
        }
        // Wait for the present-on-ready rebind to apply, then take the release/restore half against a
        // swapped store: the second record names a different world, so a leaked cache would show up as
        // the first world being presented again.
        if (!settledWorld.IsValid())
        {
            if (app.PresentedWorld() != app.BootPresented)
            {
                settledWorld = app.PresentedWorld();
                probe.Store = EncodeRecord(types, SecondGameplayKey, 0x22);
                app.ReleaseLocalSession();
                app.RestoreLocalSession();
                secondRestoreLoads = probe.Loads;
            }
            return;
        }
        if (app.PresentedWorld() != settledWorld)
        {
            secondSettledWorld = app.PresentedWorld();
            app.Quit();
        }
    };

    {
        BootRestoreApp app(std::move(info), types, systems, probe, script);
        slot = &app;
        app.Run({});

        // The gate cleared: world #0 stays presented, no rebind is even in flight, and the restore
        // path did not run at all — the record was never loaded and the factory never opened a world.
        CHECK(app.BootPresented == app.GetManagedWorldId());
        CHECK(app.BootPresented.IsValid());
        CHECK_FALSE(app.BootPending.has_value());
        CHECK(app.BootLoads == 0);
        CHECK(app.BootFactoryOpens == 0);

        // The on-demand entry: the same restore, run when the consumer asks. It reads the record and
        // requests the gameplay rebind, which then applies onto a world that is not world #0.
        CHECK(restoreLoads == 1);
        REQUIRE(restorePending.has_value());
        CHECK(*restorePending != app.GetManagedWorldId());
        CHECK(*restorePending == probe.WorldOfKey.at(FirstGameplayKey));
        REQUIRE(settledWorld.IsValid());
        CHECK(settledWorld == probe.WorldOfKey.at(FirstGameplayKey));
        CHECK(settledWorld != app.GetManagedWorldId());

        // The release half: after ReleaseLocalSession the next restore reloads through LoadSession
        // against whatever store is now open, so it lands in the second record's world, not the
        // cached first one.
        CHECK(secondRestoreLoads == 2);
        REQUIRE(secondSettledWorld.IsValid());
        CHECK(secondSettledWorld == probe.WorldOfKey.at(SecondGameplayKey));
        CHECK(secondSettledWorld != settledWorld);
    }

    // Both restores consulted the record, and the second saw the second store's pose — the reattach
    // transform runs once per restore, so this also pins that the boot did not run a third.
    CHECK(probe.Reattaches == 2);
    CHECK(probe.ReattachedPose.Bytes == vector<u8>{0x22});
}

TEST_CASE("Boot restore left at its default rebinds the managed viewport onto the recorded world")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    const path project = WriteBootstrapFixture(types, "boot_restore_on");

    RestoreProbe probe;
    probe.Store = EncodeRecord(types, FirstGameplayKey, 0x33);

    BootRestoreApp* slot = nullptr;
    // RestoreLocalSessionOnBoot is left at its default (true) — the continue-style posture.
    ApplicationInfo info = MakeInfo(project, probe, &slot);
    REQUIRE(info.World->RestoreLocalSessionOnBoot);

    WorldInstanceId settledWorld;
    const auto script = [&](BootRestoreApp& app, u32)
    {
        if (app.PresentedWorld() != app.BootPresented)
        {
            settledWorld = app.PresentedWorld();
            app.Quit();
        }
    };

    {
        BootRestoreApp app(std::move(info), types, systems, probe, script);
        slot = &app;
        app.Run({});

        // The boot ran the restore with no consumer call: the record was read and the gameplay world
        // opened before the first frame, and the rebind was already in flight then.
        CHECK(app.BootLoads == 1);
        CHECK(app.BootFactoryOpens == 1);
        CHECK(app.BootPresented == app.GetManagedWorldId());
        REQUIRE(app.BootPending.has_value());
        CHECK(*app.BootPending == probe.WorldOfKey.at(FirstGameplayKey));

        // And it applies: managed viewport 0 lands on the recorded gameplay world, not world #0.
        REQUIRE(settledWorld.IsValid());
        CHECK(settledWorld == probe.WorldOfKey.at(FirstGameplayKey));
        CHECK(settledWorld != app.GetManagedWorldId());
    }

    CHECK(probe.Reattaches == 1);
    CHECK(probe.ReattachedPose.Bytes == vector<u8>{0x33});
}
