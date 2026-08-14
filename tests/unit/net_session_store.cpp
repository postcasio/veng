// The store binding behind the session durability hooks: a record persists across a registry evict
// and reload, and across a cold reopen of the slot; two accounts differing only in the high half of
// the account id keep separate records; and a source resolving to no store leaves the registry
// memory-only, writing nothing.

#include <doctest/doctest.h>

#include <Veng/Net/AccountId.h>
#include <Veng/Net/Session.h>
#include <Veng/Persistence/SessionStore.h>
#include <Veng/Persistence/Store.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>

#include <support/TempPath.h>

#include <atomic>
#include <filesystem>

using namespace Veng;

namespace
{
    // A fresh, unique slot directory per case, removed on destruction.
    struct TempSlot
    {
        path Dir;

        TempSlot()
        {
            static std::atomic<u64> counter{0};
            Dir = TestSupport::TempDir() /
                  fmt::format("session-store-{}", counter.fetch_add(1, std::memory_order_relaxed));
            std::filesystem::remove_all(Dir);
        }

        ~TempSlot() { std::filesystem::remove_all(Dir); }
    };

    Net::Blob MakeBlob(const TypeId type, const vector<u8>& bytes)
    {
        return Net::Blob{.Type = type, .Bytes = bytes};
    }

    // The registry under test, over one type registry and one hook pair.
    Unique<Net::SessionRegistry> MakeRegistry(const TypeRegistry& types, const SessionHooks& hooks)
    {
        return Net::SessionRegistry::Create(Net::SessionRegistryInfo{
            .Types = &types, .LoadSession = hooks.LoadSession, .SaveSession = hooks.SaveSession});
    }

    // Records a standing join plus a gameplay entry, the shape every case round-trips.
    void RecordSession(Net::SessionRegistry& registry, const Net::AccountId& account,
                       const u64 standing, const u64 gameplay)
    {
        registry.RecordStandingJoin(account, Net::WorldKey::FromU64(standing));
        registry.RecordGameplay(account, Net::WorldKey::FromU64(gameplay),
                                MakeBlob(0x5E55000000000001ULL, {1, 2, 3}),
                                MakeBlob(0x5E55000000000002ULL, {4, 5}));
    }
}

TEST_CASE("A session record persists across a registry evict and reload through a store")
{
    const TempSlot slot;
    TypeRegistry types;
    RegisterBuiltinTypes(types);

    const Result<Unique<Store>> opened = Store::Open(slot.Dir);
    REQUIRE(opened.has_value());
    Store& store = **opened;
    RegisterSessionFamily(store);

    const SessionHooks hooks = MakeSessionHooks([&store] { return &store; });
    const Unique<Net::SessionRegistry> registry = MakeRegistry(types, hooks);

    const Net::AccountId account{.Lo = 0xA1A1A1A1A1A1A1A1ULL, .Hi = 0xB2B2B2B2B2B2B2B2ULL};
    RecordSession(*registry, account, 0x11, 0x22);
    registry->SaveAll();

    registry->Clear();
    CHECK(registry->Count() == 0);
    CHECK(registry->Find(account) == nullptr);

    registry->EnsureLoaded(account);
    const Net::SessionRecord* const restored = registry->Find(account);
    REQUIRE(restored != nullptr);
    CHECK(restored->Account == account);
    REQUIRE(restored->StandingJoins.size() == 1);
    CHECK(restored->StandingJoins.front() == Net::WorldKey::FromU64(0x11));
    CHECK(restored->Gameplay.Key == Net::WorldKey::FromU64(0x22));
    CHECK(restored->Gameplay.Params == MakeBlob(0x5E55000000000001ULL, {1, 2, 3}));
    CHECK(restored->Gameplay.Pose == MakeBlob(0x5E55000000000002ULL, {4, 5}));
}

TEST_CASE("A saved session record survives a cold reopen of the slot")
{
    const TempSlot slot;
    TypeRegistry types;
    RegisterBuiltinTypes(types);

    const Net::AccountId account{.Lo = 7, .Hi = 9};
    {
        const Result<Unique<Store>> opened = Store::Open(slot.Dir);
        REQUIRE(opened.has_value());
        Store& store = **opened;
        RegisterSessionFamily(store);
        const SessionHooks hooks = MakeSessionHooks([&store] { return &store; });
        const Unique<Net::SessionRegistry> registry = MakeRegistry(types, hooks);
        RecordSession(*registry, account, 0x33, 0x44);
        registry->SaveAll();
        // The default flushes on save, so the slot is already on disk when the store drops with
        // this scope (and the exclusive slot lock releases with it).
    }

    const Result<Unique<Store>> reopened = Store::Open(slot.Dir);
    REQUIRE(reopened.has_value());
    Store& store = **reopened;
    RegisterSessionFamily(store);
    const SessionHooks hooks = MakeSessionHooks([&store] { return &store; });
    const Unique<Net::SessionRegistry> registry = MakeRegistry(types, hooks);

    registry->EnsureLoaded(account);
    const Net::SessionRecord* const restored = registry->Find(account);
    REQUIRE(restored != nullptr);
    REQUIRE(restored->StandingJoins.size() == 1);
    CHECK(restored->StandingJoins.front() == Net::WorldKey::FromU64(0x33));
    CHECK(restored->Gameplay.Key == Net::WorldKey::FromU64(0x44));
}

TEST_CASE("Two accounts differing only in the high half of the id keep separate records")
{
    const TempSlot slot;
    TypeRegistry types;
    RegisterBuiltinTypes(types);

    const Result<Unique<Store>> opened = Store::Open(slot.Dir);
    REQUIRE(opened.has_value());
    Store& store = **opened;
    RegisterSessionFamily(store);
    const SessionHooks hooks = MakeSessionHooks([&store] { return &store; });
    const Unique<Net::SessionRegistry> registry = MakeRegistry(types, hooks);

    const Net::AccountId first{.Lo = 0xC0FFEEULL, .Hi = 1};
    const Net::AccountId second{.Lo = 0xC0FFEEULL, .Hi = 2};
    RecordSession(*registry, first, 0x51, 0x61);
    RecordSession(*registry, second, 0x52, 0x62);
    registry->SaveAll();
    registry->Clear();

    registry->EnsureLoaded(first);
    registry->EnsureLoaded(second);
    const Net::SessionRecord* const restoredFirst = registry->Find(first);
    const Net::SessionRecord* const restoredSecond = registry->Find(second);
    REQUIRE(restoredFirst != nullptr);
    REQUIRE(restoredSecond != nullptr);
    CHECK(restoredFirst->Gameplay.Key == Net::WorldKey::FromU64(0x61));
    CHECK(restoredSecond->Gameplay.Key == Net::WorldKey::FromU64(0x62));
    REQUIRE(restoredFirst->StandingJoins.size() == 1);
    REQUIRE(restoredSecond->StandingJoins.size() == 1);
    CHECK(restoredFirst->StandingJoins.front() == Net::WorldKey::FromU64(0x51));
    CHECK(restoredSecond->StandingJoins.front() == Net::WorldKey::FromU64(0x52));
}

TEST_CASE("A source resolving to no store leaves session records memory-only")
{
    const TempSlot slot;
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    std::filesystem::create_directories(slot.Dir);

    const SessionHooks hooks = MakeSessionHooks([]() -> Store* { return nullptr; });
    const Unique<Net::SessionRegistry> registry = MakeRegistry(types, hooks);

    const Net::AccountId account{.Lo = 5, .Hi = 5};
    RecordSession(*registry, account, 0x71, 0x81);
    CHECK(registry->Find(account) != nullptr);
    registry->SaveAll();

    registry->Clear();
    registry->EnsureLoaded(account);
    CHECK(registry->Find(account) == nullptr);
    CHECK(std::filesystem::is_empty(slot.Dir));
}

TEST_CASE("A pose capture returning nullopt keeps the recorded pose; a value overwrites it")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);

    // The hook's answer for the next capture: nullopt is "nothing newer to say".
    optional<Net::Blob> nextCapture;
    const Unique<Net::SessionRegistry> registry =
        Net::SessionRegistry::Create(Net::SessionRegistryInfo{
            .Types = &types,
            .CaptureTravelPose = [&](WorldInstanceId, Entity) { return nextCapture; },
        });

    const Net::AccountId account{.Lo = 7, .Hi = 7};
    const Net::Blob arrival = MakeBlob(0x5E55000000000003ULL, {1, 2, 3});
    registry->RecordGameplay(account, Net::WorldKey::FromU64(0x91),
                             MakeBlob(0x5E55000000000004ULL, {9}), arrival);

    registry->CaptureGameplayPose(account, WorldInstanceId{.Value = 1}, Entity::Null);
    REQUIRE(registry->Find(account) != nullptr);
    CHECK(registry->Find(account)->Gameplay.Pose == arrival);

    nextCapture = MakeBlob(0x5E55000000000005ULL, {4, 5, 6});
    registry->CaptureGameplayPose(account, WorldInstanceId{.Value = 1}, Entity::Null);
    CHECK(registry->Find(account)->Gameplay.Pose == *nextCapture);

    nextCapture.reset();
    registry->CaptureGameplayPose(account, WorldInstanceId{.Value = 1}, Entity::Null);
    CHECK(registry->Find(account)->Gameplay.Pose == MakeBlob(0x5E55000000000005ULL, {4, 5, 6}));
}
