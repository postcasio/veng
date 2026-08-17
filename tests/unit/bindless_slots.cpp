// The bindless slot-report vocabulary: the array and slot-state namers, the name/parse round trip
// an agent-facing surface accepts arguments through, and CapacityOf's coverage of the array set.
// Device-free — every one of these is a constexpr function over a closed enum, no ICD.

#include <doctest/doctest.h>

#include <Veng/Renderer/BindlessRegistry.h>

#include <set>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The closed array set, in BindlessCapacity's own order. Spelled once here so a case that means
    // "every array" cannot quietly mean "every array someone remembered".
    constexpr std::array Arrays{
        BindlessArray::Textures,  BindlessArray::Volumes,       BindlessArray::Cubes,
        BindlessArray::Samplers,  BindlessArray::StorageImages, BindlessArray::StorageBuffers,
        BindlessArray::Materials,
    };
}

TEST_CASE("Bindless: every array names distinctly and parses back")
{
    // The round trip is the property: a tool reports an array by name and accepts the same name as
    // an argument, so a name that does not parse back is a surface whose own output it rejects.
    std::set<string_view> names;
    for (const BindlessArray array : Arrays)
    {
        const string_view name = BindlessArrayName(array);
        CHECK(name != "unknown");
        CHECK(ParseBindlessArray(name) == array);
        names.insert(name);
    }
    CHECK(names.size() == Arrays.size());

    // A name matching no array is nothing, not a default — silently answering "textures" to a typo
    // would report one array's occupancy as another's.
    CHECK_FALSE(ParseBindlessArray("Textures").has_value());
    CHECK_FALSE(ParseBindlessArray("").has_value());
    CHECK_FALSE(ParseBindlessArray("storage").has_value());
}

TEST_CASE("Bindless: every array carries its own nonzero capacity")
{
    // CapacityOf is the Max* constants keyed by the enum; the claim is that each array is mapped
    // (an unmapped one answers 0) and that the mapping is not collapsed onto one constant.
    for (const BindlessArray array : Arrays)
    {
        CHECK(BindlessRegistry::CapacityOf(array) > 0u);
    }
    CHECK(BindlessRegistry::CapacityOf(BindlessArray::Textures) == BindlessRegistry::MaxTextures);
    CHECK(BindlessRegistry::CapacityOf(BindlessArray::Volumes) == BindlessRegistry::MaxVolumes);
    CHECK(BindlessRegistry::CapacityOf(BindlessArray::Cubes) == BindlessRegistry::MaxCubes);
    CHECK(BindlessRegistry::CapacityOf(BindlessArray::Samplers) == BindlessRegistry::MaxSamplers);
    CHECK(BindlessRegistry::CapacityOf(BindlessArray::StorageImages) ==
          BindlessRegistry::MaxStorageImages);
    CHECK(BindlessRegistry::CapacityOf(BindlessArray::StorageBuffers) ==
          BindlessRegistry::MaxStorageBuffers);
    CHECK(BindlessRegistry::CapacityOf(BindlessArray::Materials) == BindlessRegistry::MaxMaterials);
}

TEST_CASE("Bindless: the three slot states name distinctly")
{
    // Pending release is the state a reader cannot infer: a slot holding a live Ref that the free
    // count does not include. Naming it apart from the other two is the whole reason it exists.
    std::set<string_view> names;
    for (const BindlessSlotState state :
         {BindlessSlotState::Free, BindlessSlotState::Occupied, BindlessSlotState::PendingRelease})
    {
        const string_view name = BindlessSlotStateName(state);
        CHECK(name != "unknown");
        names.insert(name);
    }
    CHECK(names.size() == 3u);
    CHECK(BindlessSlotStateName(BindlessSlotState::PendingRelease) == "pending_release");
}

TEST_CASE("Bindless: a default slot reads as absent in every field")
{
    // Every field's zero has to read as "this array does not describe it", because the report omits
    // a field at its zero rather than emitting a misleading 0.
    const BindlessSlot slot;
    CHECK(slot.State == BindlessSlotState::Free);
    CHECK(slot.Name.empty());
    CHECK(slot.ImageFormat == Format::Undefined);
    CHECK(slot.Extent == uvec3{0, 0, 0});
    CHECK(slot.MipLevels == 0u);
    CHECK(slot.ArrayLayers == 0u);
    CHECK(slot.SizeBytes == 0u);
    CHECK(slot.ImageBytes == 0u);
}
