// VE_ENUM enumerator-table cases: a reflected enum carries its {name, value} table
// on TypeInfo, in declaration order, with the value read off the enum constant (so
// explicit / non-contiguous values reflect correctly). Device-free, no ImGui.

#include <doctest/doctest.h>

#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>

using namespace Veng;

namespace
{
    // A contiguous enum starting at zero.
    enum class Fruit : u32
    {
        Apple = 0,
        Pear = 1,
        Cherry = 2,
    };

    // An enum with an explicit, non-contiguous, non-zero-starting value set — the
    // case a positional assumption would get wrong.
    enum class Gapped : i32
    {
        Low = -3,
        Mid = 10,
        High = 100,
    };
}

VE_ENUM(::Fruit, 0x51A7ED5CA1A50001ULL)
VE_ENUMERATOR(Apple)
VE_ENUMERATOR(Pear)
VE_ENUMERATOR(Cherry)
VE_ENUM_END();

VE_ENUM(::Gapped, 0x51A7ED5CA1A50002ULL)
VE_ENUMERATOR(Low)
VE_ENUMERATOR(Mid)
VE_ENUMERATOR(High)
VE_ENUM_END();

// A bare VE_LEAF(…, Enum), un-migrated: it carries no enumerator table.
namespace
{
    enum class Bare : u32
    {
        One = 0,
    };
}

VE_LEAF(::Bare, 0x51A7ED5CA1A50003ULL, FieldClass::Enum);

TEST_CASE("A VE_ENUM type records its enumerators in declaration order")
{
    TypeRegistry registry;
    registry.Register<Fruit>();

    const TypeInfo& info = registry.Info(TypeIdOf<Fruit>());
    REQUIRE(info.Class == FieldClass::Enum);
    REQUIRE(info.Enumerators.size() == 3);

    CHECK(info.Enumerators[0].Name == "Apple");
    CHECK(info.Enumerators[0].Value == 0);
    CHECK(info.Enumerators[1].Name == "Pear");
    CHECK(info.Enumerators[1].Value == 1);
    CHECK(info.Enumerators[2].Name == "Cherry");
    CHECK(info.Enumerators[2].Value == 2);
}

TEST_CASE("A VE_ENUM reads explicit, non-contiguous values off the enum constant")
{
    TypeRegistry registry;
    registry.Register<Gapped>();

    const TypeInfo& info = registry.Info(TypeIdOf<Gapped>());
    REQUIRE(info.Enumerators.size() == 3);

    CHECK(info.Enumerators[0].Name == "Low");
    CHECK(info.Enumerators[0].Value == -3);
    CHECK(info.Enumerators[1].Name == "Mid");
    CHECK(info.Enumerators[1].Value == 10);
    CHECK(info.Enumerators[2].Name == "High");
    CHECK(info.Enumerators[2].Value == 100);
}

TEST_CASE("A bare VE_LEAF enum carries no enumerator table")
{
    TypeRegistry registry;
    registry.Register<Bare>();

    const TypeInfo& info = registry.Info(TypeIdOf<Bare>());
    CHECK(info.Class == FieldClass::Enum);
    CHECK(info.Enumerators.empty());
}
