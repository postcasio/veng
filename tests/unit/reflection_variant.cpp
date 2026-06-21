// Reflection-variant unit cases: the Variant<Ts...> value type, the VE_VARIANT
// macro, and the TypeInfo variant thunks the registry records. Pure CPU — no
// Context, no Vulkan symbol touched.

#include <doctest/doctest.h>

#include <cstring>

#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/Serialize.h>
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

    // A wrapper carrying a variant field plus a trailing scalar — the trailing
    // field proves an unknown-tag variant skip does not disturb surrounding fields.
    struct TestWrapper
    {
        TestVariant Shape;
        i32 Trailing = 0;
    };

    // A registered Struct-class type that is NOT an alternative of TestVariant —
    // the "registered but not an alternative" tolerance path.
    struct TestC
    {
        i32 Z = 0;
    };
}

VE_REFLECT(TestA, 0x2B80CC9EB3C378FEULL)
VE_FIELD(X)
VE_REFLECT_END();

VE_REFLECT(TestB, 0xFD0AFE8672B5697EULL)
VE_FIELD(Y)
VE_REFLECT_END();

VE_REFLECT(TestC, 0x72688A23DFFDA06BULL)
VE_FIELD(Z)
VE_REFLECT_END();

VE_VARIANT(TestVariant, 0x68D6F20670117323ULL);

VE_REFLECT(TestWrapper, 0xA98B5EE8914B1271ULL)
VE_FIELD(Shape)
VE_FIELD(Trailing)
VE_REFLECT_END();

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

// ---- (De)serialization through the FieldClass::Variant case ----------------

namespace
{
    // Raw little-helpers mirroring Serialize.cpp's host-byte-order encoding, used
    // to hand-build a record whose variant tag the serializer would never emit.
    void PushU32(vector<u8>& out, u32 value)
    {
        const auto* p = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), p, p + sizeof(value));
    }

    void PushU64(vector<u8>& out, u64 value)
    {
        const auto* p = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), p, p + sizeof(value));
    }

    // A name-keyed, length-prefixed field record: { u32 nameLen, name, u32 valueLen, value }.
    void PushField(vector<u8>& out, std::string_view name, const vector<u8>& value)
    {
        PushU32(out, static_cast<u32>(name.size()));
        out.insert(out.end(), name.begin(), name.end());
        PushU32(out, static_cast<u32>(value.size()));
        out.insert(out.end(), value.begin(), value.end());
    }
}

TEST_CASE("A populated variant field round-trips its active alternative and value")
{
    TypeRegistry registry;
    registry.Register<TestWrapper>();
    const TypeInfo& info = registry.Info(TypeIdOf<TestWrapper>());

    TestWrapper src;
    static_cast<TestB*>(src.Shape.SetActive(TypeIdOf<TestB>()))->Y = 3.5f;
    src.Trailing = 42;

    vector<u8> bytes;
    WriteFields(bytes, &src, info, registry);

    TestWrapper dst;
    REQUIRE(ReadFields(bytes, &dst, info, registry));

    REQUIRE(dst.Shape.ActiveType() == TypeIdOf<TestB>());
    CHECK(static_cast<const TestB*>(dst.Shape.ActivePtr())->Y == doctest::Approx(3.5f));
    CHECK(dst.Trailing == 42);
}

TEST_CASE("An empty variant field round-trips as the InvalidTypeId tag")
{
    TypeRegistry registry;
    registry.Register<TestWrapper>();
    const TypeInfo& info = registry.Info(TypeIdOf<TestWrapper>());

    TestWrapper src;
    src.Trailing = 7;

    vector<u8> bytes;
    WriteFields(bytes, &src, info, registry);

    TestWrapper dst;
    static_cast<TestA*>(dst.Shape.SetActive(TypeIdOf<TestA>()))->X = 99; // pre-populate
    REQUIRE(ReadFields(bytes, &dst, info, registry));

    CHECK_FALSE(dst.Shape.HasValue());
    CHECK(dst.Shape.ActiveType() == InvalidTypeId);
    CHECK(dst.Trailing == 7);
}

TEST_CASE("Reading a different alternative replaces, not merges, the prior one")
{
    TypeRegistry registry;
    registry.Register<TestWrapper>();
    const TypeInfo& info = registry.Info(TypeIdOf<TestWrapper>());

    TestWrapper src;
    static_cast<TestA*>(src.Shape.SetActive(TypeIdOf<TestA>()))->X = 5;

    vector<u8> bytes;
    WriteFields(bytes, &src, info, registry);

    TestWrapper dst;
    static_cast<TestB*>(dst.Shape.SetActive(TypeIdOf<TestB>()))->Y = 1.0f; // holds TestB
    REQUIRE(ReadFields(bytes, &dst, info, registry));

    REQUIRE(dst.Shape.ActiveType() == TypeIdOf<TestA>());
    CHECK(static_cast<const TestA*>(dst.Shape.ActivePtr())->X == 5);
}

TEST_CASE("An unregistered variant tag leaves the variant empty and skips its record")
{
    TypeRegistry registry;
    registry.Register<TestWrapper>();
    const TypeInfo& info = registry.Info(TypeIdOf<TestWrapper>());

    // Hand-build a wrapper record whose Shape variant carries an unregistered tag
    // plus a junk member record; Trailing follows it.
    vector<u8> variantValue;
    PushU64(variantValue, 0xDEADBEEFCAFEF00DULL); // not a registered TypeId
    PushU32(variantValue, 0);                     // a stray member record (recordCount=0)

    vector<u8> trailingValue;
    PushU32(trailingValue, 123);

    vector<u8> bytes;
    PushU32(bytes, 2); // two field records
    PushField(bytes, "Shape", variantValue);
    PushField(bytes, "Trailing", trailingValue);

    TestWrapper dst;
    REQUIRE(ReadFields(bytes, &dst, info, registry));

    CHECK_FALSE(dst.Shape.HasValue());
    CHECK(dst.Trailing == 123); // the trailing field still read correctly
}

TEST_CASE("A registered-but-not-an-alternative variant tag leaves the variant empty")
{
    TypeRegistry registry;
    registry.Register<TestWrapper>();
    registry.Register<TestC>(); // registered, but not an alternative of TestVariant
    const TypeInfo& info = registry.Info(TypeIdOf<TestWrapper>());

    // The active member record is a valid TestC record; the variant must not adopt it.
    TestC member;
    member.Z = 8;
    vector<u8> memberRecord;
    WriteFields(memberRecord, &member, registry.Info(TypeIdOf<TestC>()), registry);

    vector<u8> variantValue;
    PushU64(variantValue, TypeIdOf<TestC>());
    variantValue.insert(variantValue.end(), memberRecord.begin(), memberRecord.end());

    vector<u8> trailingValue;
    PushU32(trailingValue, 55);

    vector<u8> bytes;
    PushU32(bytes, 2);
    PushField(bytes, "Shape", variantValue);
    PushField(bytes, "Trailing", trailingValue);

    TestWrapper dst;
    REQUIRE(ReadFields(bytes, &dst, info, registry));

    CHECK_FALSE(dst.Shape.HasValue());
    CHECK(dst.Trailing == 55);
}

TEST_CASE("A truncated variant tag is a recoverable error")
{
    TypeRegistry registry;
    registry.Register<TestWrapper>();
    const TypeInfo& info = registry.Info(TypeIdOf<TestWrapper>());

    // A Shape value of only 4 bytes — short of the 8-byte tag.
    vector<u8> variantValue;
    PushU32(variantValue, 0);

    vector<u8> bytes;
    PushU32(bytes, 1);
    PushField(bytes, "Shape", variantValue);

    TestWrapper dst;
    const VoidResult result = ReadFields(bytes, &dst, info, registry);
    CHECK_FALSE(result);
}
