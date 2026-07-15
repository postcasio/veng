// The GuiDriverRegistry — the per-instance UI presentation driver catalog, mirroring SystemRegistry.
// Pure CPU: registration and instantiation touch no Context/device (the GPU-free/cooker-safe
// contract), so these run with no ICD.

#include <doctest/doctest.h>

#include <Veng/Gui/Driver.h>
#include <Veng/Gui/DriverRegistry.h>

using namespace Veng;

namespace
{
    // A trivial driver whose OnUpdate bumps a per-instance counter, so two instances are provably
    // independent. It reads nothing and stamps nothing — enough to exercise the catalog + lifecycle.
    struct CounterDriver final : GuiDriver
    {
        int Updates = 0;
        void OnUpdate(const GuiDriverFrame&) override { ++Updates; }
    };

    struct OtherDriver final : GuiDriver
    {
    };
}

VE_GUI_DRIVER(CounterDriver, 0x1111000000000001ULL, "Counter");
VE_GUI_DRIVER(OtherDriver, 0x1111000000000002ULL, "Other");

TEST_CASE("GuiDriverRegistry enumerates without instantiating and resolves an id to a driver")
{
    GuiDriverRegistry registry;
    CHECK(registry.Count() == 0);

    registry.Register<CounterDriver>();
    registry.Register<OtherDriver>();

    REQUIRE(registry.Count() == 2);

    // The catalog carries identity + name in registration order, without building any driver.
    const vector<GuiDriverEntry>& entries = registry.Entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].Id == GuiDriverIdOf<CounterDriver>());
    CHECK(entries[0].Name == "Counter");
    CHECK(entries[1].Id == GuiDriverIdOf<OtherDriver>());

    // Resolving an id builds one; an unknown id yields nullptr (the undriven fallback).
    const Unique<GuiDriver> built = registry.Instantiate(GuiDriverIdOf<CounterDriver>());
    CHECK(built != nullptr);
    CHECK(registry.Instantiate(static_cast<GuiDriverId>(0xDEADBEEFULL)) == nullptr);
}

TEST_CASE("Two instantiations of one driver are independent — the split-screen guarantee")
{
    GuiDriverRegistry registry;
    registry.Register<CounterDriver>();

    // Two claimed overlay instances (split-screen) are two driver instances with their own state.
    const Unique<GuiDriver> a = registry.Instantiate(GuiDriverIdOf<CounterDriver>());
    const Unique<GuiDriver> b = registry.Instantiate(GuiDriverIdOf<CounterDriver>());
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a.get() != b.get());

    // Mutating one instance's view-model state leaves the other's untouched — the per-instance
    // guarantee that dissolves the entity-keyed state a per-world binding system would carry.
    static_cast<CounterDriver*>(a.get())->Updates = 2;
    static_cast<CounterDriver*>(b.get())->Updates = 5;
    CHECK(static_cast<CounterDriver*>(a.get())->Updates == 2);
    CHECK(static_cast<CounterDriver*>(b.get())->Updates == 5);
}
