// Reflection-variant unit cases: the Variant<Ts...> value type, the VE_VARIANT
// macro, and the TypeInfo variant thunks the registry records. Pure CPU — no
// Context, no Vulkan symbol touched.

#include <doctest/doctest.h>

#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Reflection/Variant.h>

using namespace Veng;

// ---- Throwaway reflected alternatives + a variant over them -----------------

namespace
{
    struct TestA
    {
        i32 X = 0;
    };

    struct TestB
    {
        f32 Y = 0.0f;
    };

    using TestVariant = Variant<TestA, TestB>;
}

VE_REFLECT(TestA, 0x2B80CC9EB3C378FEULL)
VE_FIELD(X)
VE_REFLECT_END();

VE_REFLECT(TestB, 0xFD0AFE8672B5697EULL)
VE_FIELD(Y)
VE_REFLECT_END();

VE_VARIANT(TestVariant, 0x68D6F20670117323ULL);

// ---- Trait resolution ------------------------------------------------------

namespace
{
    static_assert(FieldClassOf<TestVariant>() == FieldClass::Variant);
    static_assert(TypeIdOf<TestVariant>() == 0x68D6F20670117323ULL);
}

// ---- Variant<Ts...> direct member behaviour --------------------------------

TEST_CASE("A default Variant is empty")
{
    TestVariant v;
    CHECK_FALSE(v.HasValue());
    CHECK(v.ActiveType() == InvalidTypeId);
    CHECK(v.ActivePtr() == nullptr);

    const TestVariant& cv = v;
    CHECK(cv.ActivePtr() == nullptr);
}

TEST_CASE("SetActive activates an alternative and writes through the pointer")
{
    TestVariant v;
    void* slot = v.SetActive(TypeIdOf<TestB>());
    REQUIRE(slot != nullptr);
    CHECK(v.HasValue());
    CHECK(v.ActiveType() == TypeIdOf<TestB>());

    static_cast<TestB*>(slot)->Y = 4.25f;
    CHECK(static_cast<const TestB*>(v.ActivePtr())->Y == doctest::Approx(4.25f));
}

TEST_CASE("SetActive of an unknown id is a no-op leaving the prior state")
{
    TestVariant v;
    v.SetActive(TypeIdOf<TestA>());
    static_cast<TestA*>(v.ActivePtr())->X = 9;

    const void* slot = v.SetActive(0xDEADBEEFDEADBEEFULL); // not an alternative
    CHECK(slot == nullptr);
    CHECK(v.ActiveType() == TypeIdOf<TestA>());
    CHECK(static_cast<const TestA*>(v.ActivePtr())->X == 9);
}

TEST_CASE("Clear returns a populated Variant to empty")
{
    TestVariant v;
    v.SetActive(TypeIdOf<TestA>());
    REQUIRE(v.HasValue());

    v.Clear();
    CHECK_FALSE(v.HasValue());
    CHECK(v.ActiveType() == InvalidTypeId);
    CHECK(v.ActivePtr() == nullptr);
}

TEST_CASE("Alternatives lists the alternatives in declaration order")
{
    const std::span<const TypeId> alts = TestVariant::Alternatives();
    REQUIRE(alts.size() == 2);
    CHECK(alts[0] == TypeIdOf<TestA>());
    CHECK(alts[1] == TypeIdOf<TestB>());
}

// ---- The TypeInfo variant thunks the registry records ----------------------

TEST_CASE("Register<Variant> records the variant thunks and auto-registers its alternatives")
{
    TypeRegistry registry;
    registry.Register<TestVariant>();

    // The variant and both alternatives are registered with no manual ordering.
    CHECK(registry.IsRegistered(TypeIdOf<TestVariant>()));
    CHECK(registry.IsRegistered(TypeIdOf<TestA>()));
    CHECK(registry.IsRegistered(TypeIdOf<TestB>()));

    const TypeInfo& info = registry.Info(TypeIdOf<TestVariant>());
    CHECK(info.Class == FieldClass::Variant);
    REQUIRE(info.VariantActiveType != nullptr);
    REQUIRE(info.VariantActivePtr != nullptr);
    REQUIRE(info.VariantActivePtrConst != nullptr);
    REQUIRE(info.VariantSetActive != nullptr);
    REQUIRE(info.VariantClear != nullptr);

    REQUIRE(info.VariantAlternatives.size() == 2);
    CHECK(info.VariantAlternatives[0] == TypeIdOf<TestA>());
    CHECK(info.VariantAlternatives[1] == TypeIdOf<TestB>());

    // Drive the thunks against a real variant through erased pointers.
    TestVariant v;
    CHECK(info.VariantActiveType(&v) == InvalidTypeId);
    CHECK(info.VariantActivePtr(&v) == nullptr);

    void* slot = info.VariantSetActive(&v, TypeIdOf<TestB>());
    REQUIRE(slot != nullptr);
    static_cast<TestB*>(slot)->Y = 1.5f;
    CHECK(info.VariantActiveType(&v) == TypeIdOf<TestB>());

    const void* cslot = info.VariantActivePtrConst(&v);
    REQUIRE(cslot != nullptr);
    CHECK(static_cast<const TestB*>(cslot)->Y == doctest::Approx(1.5f));

    CHECK(info.VariantSetActive(&v, 0x0123456789ABCDEFULL) == nullptr);
    CHECK(info.VariantActiveType(&v) == TypeIdOf<TestB>()); // unchanged

    info.VariantClear(&v);
    CHECK(info.VariantActiveType(&v) == InvalidTypeId);
}

TEST_CASE("A non-variant type carries null variant thunks")
{
    TypeRegistry registry;
    registry.Register<TestA>();

    const TypeInfo& info = registry.Info(TypeIdOf<TestA>());
    CHECK(info.Class == FieldClass::Struct);
    CHECK(info.VariantActiveType == nullptr);
    CHECK(info.VariantActivePtr == nullptr);
    CHECK(info.VariantActivePtrConst == nullptr);
    CHECK(info.VariantSetActive == nullptr);
    CHECK(info.VariantClear == nullptr);
    CHECK(info.VariantAlternatives.empty());
}

TEST_CASE("The variant's MoveConstruct/Destruct thunks round-trip a populated value")
{
    TypeRegistry registry;
    registry.Register<TestVariant>();
    const TypeInfo& info = registry.Info(TypeIdOf<TestVariant>());

    TestVariant src;
    static_cast<TestB*>(src.SetActive(TypeIdOf<TestB>()))->Y = 7.0f;

    // Move src into raw storage through the recorded thunk, read it back.
    alignas(TestVariant) std::byte storage[sizeof(TestVariant)];
    info.MoveConstruct(&storage, &src);

    auto* moved = reinterpret_cast<TestVariant*>(&storage);
    CHECK(moved->ActiveType() == TypeIdOf<TestB>());
    CHECK(static_cast<const TestB*>(moved->ActivePtr())->Y == doctest::Approx(7.0f));

    info.Destruct(&storage);
}
