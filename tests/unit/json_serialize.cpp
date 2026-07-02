// JSON<->reflection walker unit cases: JsonReadFields/JsonWriteFields (Veng/Reflection/
// JsonSerialize.h) over a fixture type exercising every FieldClass, the enum-by-name
// convention, the merge-write form, and dotted-path error content. Pure CPU — no Context,
// no Vulkan symbol touched.

#include <doctest/doctest.h>

#include <cstring>

#include <Veng/Reflection/EnumName.h>
#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Reflection/Variant.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>

using namespace Veng;
using Json = nlohmann::json;

// ---- Fixture types: one FieldClass each -------------------------------------

namespace
{
    enum class Tier : u32
    {
        Bronze = 0,
        Silver = 1,
        Gold = 2,
    };

    struct Nested
    {
        f32 Amount = 0.0f;
        string Label;
    };

    struct VariantA
    {
        i32 X = 0;
    };

    struct VariantB
    {
        f32 Y = 0.0f;
    };

    using ShapeVariant = Variant<VariantA, VariantB>;

    // Exercises every FieldClass in one type: Scalar (several leaf widths),
    // Vector/Quaternion/Matrix, String, Enum, AssetHandle, Reference, Struct,
    // Variant, Array.
    struct Fixture
    {
        bool Flag = false;
        f32 Amount = 0.0f;
        i32 Count = 0;
        u32 Index = 0;
        u64 BigId = 0;
        vec3 Position{0.0f};
        quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
        mat4 Transform{1.0f};
        string Name;
        Tier Rank = Tier::Bronze;
        AssetHandle<Mesh> MeshRef;
        Entity Target = Entity::Null;
        Nested Sub;
        ShapeVariant Shape;
        vector<i32> Numbers;
    };
}

VE_ENUM(::Tier, 0x5A1B2C3D4E5F0001ULL)
VE_ENUMERATOR(Bronze)
VE_ENUMERATOR(Silver)
VE_ENUMERATOR(Gold)
VE_ENUM_END();

VE_REFLECT(::Nested, 0x5A1B2C3D4E5F0002ULL)
VE_FIELD(Amount)
VE_FIELD(Label)
VE_REFLECT_END();

VE_REFLECT(::VariantA, 0x5A1B2C3D4E5F0003ULL)
VE_FIELD(X)
VE_REFLECT_END();

VE_REFLECT(::VariantB, 0x5A1B2C3D4E5F0004ULL)
VE_FIELD(Y)
VE_REFLECT_END();

VE_VARIANT(::ShapeVariant, 0x5A1B2C3D4E5F0005ULL);

VE_REFLECT(::Fixture, 0x5A1B2C3D4E5F0006ULL)
VE_FIELD(Flag)
VE_FIELD(Amount)
VE_FIELD(Count)
VE_FIELD(Index)
VE_FIELD(BigId)
VE_FIELD(Position)
VE_FIELD(Rotation)
VE_FIELD(Transform)
VE_FIELD(Name)
VE_FIELD(Rank)
VE_FIELD(MeshRef)
VE_FIELD(Target)
VE_FIELD(Sub)
VE_FIELD(Shape)
VE_ARRAY_FIELD(Numbers)
VE_REFLECT_END();

namespace
{
    TypeRegistry MakeRegistry()
    {
        TypeRegistry registry;
        registry.Register<Fixture>();
        return registry;
    }

    // A stub Reference hook pair addressing entities by their raw Index, sufficient
    // for a round-trip test with no live Scene on the read/write side symmetrically.
    JsonFieldHooks StubHooks()
    {
        JsonFieldHooks hooks;
        hooks.ReadReference = [](const Json& value) -> Result<Entity>
        {
            if (!value.is_number_unsigned())
            {
                return std::unexpected(string("expected an unsigned entity index"));
            }
            return Entity{.Index = value.get<u32>(), .Generation = 0};
        };
        hooks.WriteReference = [](Entity entity) -> Json { return entity.Index; };
        return hooks;
    }

    bool FieldwiseEqual(const Fixture& a, const Fixture& b)
    {
        if (a.Flag != b.Flag || a.Count != b.Count || a.Index != b.Index || a.BigId != b.BigId)
        {
            return false;
        }
        if (a.Amount != doctest::Approx(b.Amount))
        {
            return false;
        }
        if (a.Position != b.Position)
        {
            return false;
        }
        if (a.Rotation.w != doctest::Approx(b.Rotation.w) ||
            a.Rotation.x != doctest::Approx(b.Rotation.x) ||
            a.Rotation.y != doctest::Approx(b.Rotation.y) ||
            a.Rotation.z != doctest::Approx(b.Rotation.z))
        {
            return false;
        }
        if (a.Transform != b.Transform)
        {
            return false;
        }
        if (a.Name != b.Name || a.Rank != b.Rank)
        {
            return false;
        }
        u64 aId = 0;
        u64 bId = 0;
        std::memcpy(&aId, static_cast<const void*>(&a.MeshRef), sizeof(aId));
        std::memcpy(&bId, static_cast<const void*>(&b.MeshRef), sizeof(bId));
        if (aId != bId)
        {
            return false;
        }
        if (a.Target != b.Target)
        {
            return false;
        }
        if (a.Sub.Amount != doctest::Approx(b.Sub.Amount) || a.Sub.Label != b.Sub.Label)
        {
            return false;
        }
        if (a.Shape.ActiveType() != b.Shape.ActiveType())
        {
            return false;
        }
        if (a.Shape.ActiveType() == TypeIdOf<VariantA>())
        {
            if (static_cast<const VariantA*>(a.Shape.ActivePtr())->X !=
                static_cast<const VariantA*>(b.Shape.ActivePtr())->X)
            {
                return false;
            }
        }
        else if (a.Shape.ActiveType() == TypeIdOf<VariantB>())
        {
            if (static_cast<const VariantB*>(a.Shape.ActivePtr())->Y !=
                doctest::Approx(static_cast<const VariantB*>(b.Shape.ActivePtr())->Y))
            {
                return false;
            }
        }
        if (a.Numbers != b.Numbers)
        {
            return false;
        }
        return true;
    }
}

// ---- Full-coverage round-trip -----------------------------------------------

TEST_CASE("Every FieldClass round-trips through JsonWriteFields -> JsonReadFields")
{
    const TypeRegistry registry = MakeRegistry();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<Fixture>());

    Fixture src;
    src.Flag = true;
    src.Amount = 3.5f;
    src.Count = -7;
    src.Index = 42;
    src.BigId = 0xABCDEF0123456789ULL;
    src.Position = vec3{1.0f, 2.0f, 3.0f};
    src.Rotation = quat{0.7071f, 0.0f, 0.7071f, 0.0f};
    src.Transform = mat4{2.0f};
    src.Name = "hero";
    src.Rank = Tier::Gold;
    const u64 meshId = 0x1122334455667788ULL;
    std::memcpy(static_cast<void*>(&src.MeshRef), &meshId, sizeof(meshId));
    src.Target = Entity{.Index = 5, .Generation = 0};
    src.Sub = Nested{.Amount = 9.5f, .Label = "inner"};
    static_cast<VariantB*>(src.Shape.SetActive(TypeIdOf<VariantB>()))->Y = 12.25f;
    src.Numbers = {1, 2, 3, -4};

    const Json doc = JsonWriteFields(&src, info, registry, hooks);

    Fixture dst;
    const VoidResult result = JsonReadFields(&dst, info, doc, registry, hooks);
    REQUIRE(result);
    CHECK(FieldwiseEqual(src, dst));
}

TEST_CASE("An empty variant is omitted on write and an empty array round-trips")
{
    const TypeRegistry registry = MakeRegistry();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<Fixture>());

    Fixture src;
    const Json doc = JsonWriteFields(&src, info, registry, hooks);

    // An empty variant is omitted from the written document entirely — schema-drift
    // tolerance then keeps whatever the destination already held, exactly as an
    // omitted Scalar/Struct field does.
    CHECK_FALSE(doc.contains("Shape"));

    Fixture dst;
    static_cast<VariantA*>(dst.Shape.SetActive(TypeIdOf<VariantA>()))->X = 99; // pre-populate
    REQUIRE(JsonReadFields(&dst, info, doc, registry, hooks));
    CHECK(dst.Shape.HasValue());
    CHECK(static_cast<const VariantA*>(dst.Shape.ActivePtr())->X == 99);
    CHECK(dst.Numbers.empty());
}

TEST_CASE("An explicit empty-string variant type clears the destination")
{
    const TypeRegistry registry = MakeRegistry();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<Fixture>());

    Json doc = Json::object();
    Json shape = Json::object();
    shape["type"] = "";
    doc["Shape"] = shape;

    Fixture dst;
    static_cast<VariantA*>(dst.Shape.SetActive(TypeIdOf<VariantA>()))->X = 99; // pre-populate
    REQUIRE(JsonReadFields(&dst, info, doc, registry, hooks));
    CHECK_FALSE(dst.Shape.HasValue());
}

TEST_CASE("A null AssetHandle and null Reference round-trip as JSON null")
{
    const TypeRegistry registry = MakeRegistry();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<Fixture>());

    Fixture src; // MeshRef and Target default to "no asset" / Entity::Null
    const Json doc = JsonWriteFields(&src, info, registry, hooks);
    CHECK(doc["Target"].is_null());

    Fixture dst;
    dst.Target = Entity{.Index = 3, .Generation = 1}; // pre-populate, must be overwritten
    REQUIRE(JsonReadFields(&dst, info, doc, registry, hooks));
    CHECK(dst.Target == Entity::Null);
}

// ---- Enum cases pinned ------------------------------------------------------

TEST_CASE("Enum write emits the exact enumerator spelling")
{
    const TypeRegistry registry = MakeRegistry();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<Fixture>());

    Fixture src;
    src.Rank = Tier::Silver;
    const Json doc = JsonWriteFields(&src, info, registry, hooks);
    CHECK(doc["Rank"] == "Silver");
}

TEST_CASE("Enum read matches an exact enumerator name")
{
    const TypeRegistry registry = MakeRegistry();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<Fixture>());

    Json doc = Json::object();
    doc["Rank"] = "Gold";

    Fixture dst;
    REQUIRE(JsonReadFields(&dst, info, doc, registry, hooks));
    CHECK(dst.Rank == Tier::Gold);
}

TEST_CASE("Enum read rejects an unknown enumerator name")
{
    const TypeRegistry registry = MakeRegistry();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<Fixture>());

    Json doc = Json::object();
    doc["Rank"] = "Platinum";

    Fixture dst;
    const VoidResult result = JsonReadFields(&dst, info, doc, registry, hooks);
    REQUIRE_FALSE(result);
    CHECK(result.error().find("Rank") != string::npos);
}

TEST_CASE("Enum read rejects an integer — the hard cut, no tolerance")
{
    const TypeRegistry registry = MakeRegistry();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<Fixture>());

    Json doc = Json::object();
    doc["Rank"] = 2; // Gold's ordinal — must still be rejected as not a string

    Fixture dst;
    const VoidResult result = JsonReadFields(&dst, info, doc, registry, hooks);
    REQUIRE_FALSE(result);
    CHECK(result.error().find("Rank") != string::npos);
}

TEST_CASE("An out-of-range enum value writes its decimal fallback and fails to re-read")
{
    const TypeRegistry& registryEnum = []() -> const TypeRegistry&
    {
        static TypeRegistry registry;
        static bool registered = false;
        if (!registered)
        {
            registry.Register<Tier>();
            registered = true;
        }
        return registry;
    }();

    const TypeInfo& tierInfo = registryEnum.Info(TypeIdOf<Tier>());

    // A value matching no enumerator: EnumeratorName falls back to its decimal string.
    const string fallback = EnumeratorName(tierInfo, 99);
    CHECK(fallback == "99");

    // The documented asymmetry: that decimal string is not itself a valid enumerator
    // name, so ParseEnumValue (and thus the walker's read side) rejects it.
    CHECK_FALSE(ParseEnumValue(tierInfo, fallback).has_value());
}

// ---- Merge-write pinned -----------------------------------------------------

TEST_CASE("Merge-write leaves an unknown key untouched while updating reflected fields")
{
    const TypeRegistry registry = MakeRegistry();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<Fixture>());

    Json doc = Json::object();
    doc["_comment"] = "hand-authored note";
    doc["Name"] = "stale";

    Fixture src;
    src.Name = "fresh";
    JsonWriteFields(doc, &src, info, registry, hooks);

    CHECK(doc["_comment"] == "hand-authored note");
    CHECK(doc["Name"] == "fresh");
}

TEST_CASE("Strict JsonReadFields errors on an unknown key by default")
{
    const TypeRegistry registry = MakeRegistry();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<Fixture>());

    Json doc = Json::object();
    doc["NotAField"] = 1;

    Fixture dst;
    const VoidResult result = JsonReadFields(&dst, info, doc, registry, hooks);
    REQUIRE_FALSE(result);
}

TEST_CASE("allowUnknownFields=true ignores an unknown key")
{
    const TypeRegistry registry = MakeRegistry();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<Fixture>());

    Json doc = Json::object();
    doc["NotAField"] = 1;
    doc["Name"] = "tolerant";

    Fixture dst;
    const VoidResult result = JsonReadFields(&dst, info, doc, registry, hooks, true);
    REQUIRE(result);
    CHECK(dst.Name == "tolerant");
}

// ---- Dotted-path error content pinned ---------------------------------------

TEST_CASE("A nested struct field failure names the dotted field path")
{
    const TypeRegistry registry = MakeRegistry();
    const JsonFieldHooks hooks = StubHooks();
    const TypeInfo& info = registry.Info(registry.IdOf<Fixture>());

    Json doc = Json::object();
    Json sub = Json::object();
    sub["Amount"] = "not-a-number"; // Nested.Amount is f32 — a located type error
    doc["Sub"] = sub;

    Fixture dst;
    const VoidResult result = JsonReadFields(&dst, info, doc, registry, hooks);
    REQUIRE_FALSE(result);
    CHECK(result.error().find("Sub.Amount") != string::npos);
}
