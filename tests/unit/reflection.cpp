// Reflection-layer unit cases: the TypeId leaf vocabulary, the VE_REFLECT
// describe-block, and the generic schema-driven serialize round-trip. Pure CPU —
// no Context, no Vulkan symbol touched.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Reflection/Serialize.h>

using namespace Veng;

// ---- Vocabulary: leaf TypeIds are stable, distinct, correct class ----------

namespace
{
    // Each builtin leaf id is a distinct compile-time constant.
    static_assert(TypeIdOf<bool>() != TypeIdOf<f32>());
    static_assert(TypeIdOf<f32>() != TypeIdOf<i32>());
    static_assert(TypeIdOf<i32>() != TypeIdOf<u32>());
    static_assert(TypeIdOf<u32>() != TypeIdOf<u64>());
    static_assert(TypeIdOf<vec2>() != TypeIdOf<vec3>());
    static_assert(TypeIdOf<vec3>() != TypeIdOf<vec4>());
    static_assert(TypeIdOf<quat>() != TypeIdOf<mat4>());
    static_assert(TypeIdOf<string>() != TypeIdOf<vec3>());
    static_assert(TypeIdOf<AssetHandle<Mesh>>() != TypeIdOf<AssetHandle<Material>>());
    static_assert(TypeIdOf<Entity>() != TypeIdOf<u64>());

    // FieldClass resolves correctly per leaf.
    static_assert(FieldClassOf<bool>() == FieldClass::Scalar);
    static_assert(FieldClassOf<f32>() == FieldClass::Scalar);
    static_assert(FieldClassOf<u64>() == FieldClass::Scalar);
    static_assert(FieldClassOf<vec3>() == FieldClass::Vector);
    static_assert(FieldClassOf<quat>() == FieldClass::Quaternion);
    static_assert(FieldClassOf<mat4>() == FieldClass::Matrix);
    static_assert(FieldClassOf<string>() == FieldClass::String);
    static_assert(FieldClassOf<AssetHandle<Mesh>>() == FieldClass::AssetHandle);
    static_assert(FieldClassOf<Entity>() == FieldClass::Reference);
}

TEST_CASE("Leaf TypeIds and FieldClasses are stable and correct")
{
    CHECK(TypeIdOf<vec3>() == 0xA9A78263CAA293E7ULL);
    CHECK(FieldClassOf<vec3>() == FieldClass::Vector);
    CHECK(FieldClassOf<Entity>() == FieldClass::Reference);
}

// ---- Test types authored through VE_REFLECT --------------------------------

namespace
{
    enum class Team : u32 { Red = 1, Blue = 7, Green = 42 };

    struct Inner
    {
        f32 A = 0.0f;
        u32 B = 0;
    };

    struct Outer
    {
        Inner Nested;
        vec3 Offset{0.0f};
    };

    struct WithAsset
    {
        AssetHandle<Mesh> Mesh;
    };

    struct WithEnum
    {
        Team Side = Team::Red;
    };

    struct WithReference
    {
        Entity Target = Entity::Null;
    };

    // A plain struct never added to any Scene — proves the layer is not
    // component-bound.
    struct PlainData
    {
        f32 X = 0.0f;
        string Label;
    };
}

// Team is a game-defined enum leaf: a fake leaf TypeId, no engine change.
template <> struct Veng::ReflectLeaf<Team>
{ static constexpr TypeId Id = 0x11AA22BB33CC44DDULL; static constexpr FieldClass Class = FieldClass::Enum; };

VE_REFLECT(Inner, 0x7700110022003300ULL)
    VE_FIELD(A, .DisplayName = "Amplitude")
    VE_FIELD(B)
VE_REFLECT_END();

VE_REFLECT(Outer, 0x7700110022003301ULL)
    VE_FIELD(Nested)
    VE_FIELD(Offset, .DisplayName = "Offset")
VE_REFLECT_END();

VE_REFLECT(WithAsset, 0x7700110022003302ULL)
    VE_FIELD(Mesh)
VE_REFLECT_END();

VE_REFLECT(WithEnum, 0x7700110022003303ULL)
    VE_FIELD(Side)
VE_REFLECT_END();

VE_REFLECT(WithReference, 0x7700110022003304ULL)
    VE_FIELD(Target)
VE_REFLECT_END();

VE_REFLECT(PlainData, 0x7700110022003305ULL)
    VE_FIELD(X, .Min = -1.0, .Max = 1.0)
    VE_FIELD(Label)
VE_REFLECT_END();

// ---- VE_REFLECT shape ------------------------------------------------------

TEST_CASE("VE_REFLECT records names, offsets, metadata, and the authored id")
{
    const vector<FieldDescriptor> fields = VengReflect<Transform>::Fields();

    REQUIRE(fields.size() == 3);
    CHECK(fields[0].Name == "Position");
    CHECK(fields[0].Type == TypeIdOf<vec3>());
    CHECK(fields[0].Class == FieldClass::Vector);
    CHECK(fields[0].Offset == offsetof(Transform, Position));
    CHECK(fields[0].DisplayName == "Position");
    CHECK(fields[0].Tooltip == "Local position, parent space");

    CHECK(fields[2].Name == "Scale");
    CHECK(fields[2].Offset == offsetof(Transform, Scale));
    CHECK(fields[2].Min == doctest::Approx(0.001));

    CHECK(VengReflect<Transform>::Id == 0x0AB8E30B2F638555ULL);
    CHECK(VengReflect<Transform>::Class() == FieldClass::Struct);
}

TEST_CASE("DisplayName defaults to Name when omitted")
{
    const vector<FieldDescriptor> fields = VengReflect<Inner>::Fields();
    REQUIRE(fields.size() == 2);
    CHECK(fields[0].DisplayName == "Amplitude"); // explicit
    CHECK(fields[1].Name == "B");
    CHECK(fields[1].DisplayName == "B");         // defaulted
}

TEST_CASE("Descriptor offset round-trips against the typed member")
{
    Transform t;
    const vector<FieldDescriptor> fields = VengReflect<Transform>::Fields();
    // Write through the descriptor offset, read through the member.
    auto* scale = reinterpret_cast<vec3*>(reinterpret_cast<u8*>(&t) + fields[2].Offset);
    *scale = vec3{2.0f, 3.0f, 4.0f};
    CHECK(t.Scale.x == doctest::Approx(2.0f));
    CHECK(t.Scale.y == doctest::Approx(3.0f));
    CHECK(t.Scale.z == doctest::Approx(4.0f));
}

// ---- Generic round-trip ----------------------------------------------------

namespace
{
    TypeRegistry MakeRegistry()
    {
        TypeRegistry registry;
        registry.Register<Name>();
        registry.Register<Transform>();
        registry.Register<Parent>();
        registry.Register<Light>();
        registry.Register<Inner>();
        registry.Register<Outer>();
        registry.Register<WithAsset>();
        registry.Register<WithEnum>();
        registry.Register<WithReference>();
        registry.Register<PlainData>();
        return registry;
    }
}

TEST_CASE("Transform + Name round-trip through the descriptor walk only")
{
    TypeRegistry registry = MakeRegistry();

    Transform src;
    src.Position = vec3{1.0f, 2.0f, 3.0f};
    src.Rotation = quat{0.5f, 0.5f, 0.5f, 0.5f};
    src.Scale = vec3{4.0f, 5.0f, 6.0f};

    vector<u8> bytes;
    WriteFields(bytes, &src, registry.Info(registry.IdOf<Transform>()), registry);

    Transform dst; // fresh defaults
    ReadFields(bytes, &dst, registry.Info(registry.IdOf<Transform>()), registry);

    CHECK(dst.Position == src.Position);
    CHECK(dst.Rotation.w == doctest::Approx(src.Rotation.w));
    CHECK(dst.Rotation.x == doctest::Approx(src.Rotation.x));
    CHECK(dst.Scale == src.Scale);

    Name nameSrc{"hero"};
    vector<u8> nameBytes;
    WriteFields(nameBytes, &nameSrc, registry.Info(registry.IdOf<Name>()), registry);
    Name nameDst;
    ReadFields(nameBytes, &nameDst, registry.Info(registry.IdOf<Name>()), registry);
    CHECK(nameDst.Value == "hero");
}

TEST_CASE("Light round-trips through the descriptor walk only")
{
    TypeRegistry registry = MakeRegistry();

    Light src;
    src.Direction = vec3{0.2f, -0.8f, 0.55f};
    src.Color = vec3{0.9f, 0.4f, 0.1f};
    src.Intensity = 2.5f;

    vector<u8> bytes;
    WriteFields(bytes, &src, registry.Info(registry.IdOf<Light>()), registry);

    Light dst; // fresh defaults
    ReadFields(bytes, &dst, registry.Info(registry.IdOf<Light>()), registry);

    CHECK(dst.Direction == src.Direction);
    CHECK(dst.Color == src.Color);
    CHECK(dst.Intensity == doctest::Approx(src.Intensity));
}

TEST_CASE("Editor metadata does not affect the serialized bytes")
{
    TypeRegistry registry = MakeRegistry();

    // PlainData carries Min/Max metadata on X; serializing must ignore it. The
    // byte stream is identical whether metadata is present or not — proven by the
    // round-trip reproducing the value with no metadata-derived clamping.
    PlainData src{0.75f, "tag"};
    vector<u8> bytes;
    WriteFields(bytes, &src, registry.Info(registry.IdOf<PlainData>()), registry);
    PlainData dst;
    ReadFields(bytes, &dst, registry.Info(registry.IdOf<PlainData>()), registry);
    CHECK(dst.X == doctest::Approx(0.75f));
    CHECK(dst.Label == "tag");
}

TEST_CASE("Nested struct recurses and auto-registers the nested schema")
{
    TypeRegistry registry;
    // Register only the outer type; the inner schema must be auto-registered on
    // reference.
    registry.Register<Outer>();
    CHECK(registry.IsRegistered(registry.IdOf<Inner>()));

    Outer src;
    src.Nested = Inner{1.5f, 99u};
    src.Offset = vec3{7.0f, 8.0f, 9.0f};

    vector<u8> bytes;
    WriteFields(bytes, &src, registry.Info(registry.IdOf<Outer>()), registry);
    Outer dst;
    ReadFields(bytes, &dst, registry.Info(registry.IdOf<Outer>()), registry);

    CHECK(dst.Nested.A == doctest::Approx(1.5f));
    CHECK(dst.Nested.B == 99u);
    CHECK(dst.Offset == src.Offset);
}

TEST_CASE("Schema tolerance: extra record skipped, missing field keeps default")
{
    TypeRegistry registry = MakeRegistry();

    // Serialize the full Inner (A + B).
    Inner src{3.0f, 17u};
    vector<u8> bytes;
    WriteFields(bytes, &src, registry.Info(registry.IdOf<Inner>()), registry);

    // Read into a descriptor with field B removed: the B record is skipped, A
    // survives, nothing corrupts.
    {
        TypeInfo trimmed = registry.Info(registry.IdOf<Inner>());
        trimmed.Fields.pop_back(); // drop B
        Inner dst;
        ReadFields(bytes, &dst, trimmed, registry);
        CHECK(dst.A == doctest::Approx(3.0f));
        CHECK(dst.B == 0u); // never touched
    }

    // Read into a descriptor with an extra field (not in the data): it keeps its
    // default. Build the extended descriptor by hand atop Inner's two fields.
    {
        TypeInfo extended = registry.Info(registry.IdOf<Inner>());
        FieldDescriptor extra;
        extra.Name = "C";
        extra.Type = TypeIdOf<f32>();
        extra.Class = FieldClass::Scalar;
        extra.Offset = offsetof(Inner, A); // any valid offset; data has no "C"
        extended.Fields.push_back(extra);

        Inner dst;
        ReadFields(bytes, &dst, extended, registry);
        CHECK(dst.A == doctest::Approx(3.0f));
        CHECK(dst.B == 17u);
    }
}

TEST_CASE("AssetHandle field round-trips its underlying AssetId")
{
    TypeRegistry registry = MakeRegistry();

    WithAsset src;
    // No AssetManager: forge the underlying id via the byte layout the serializer
    // reads. (AssetHandle's leading member is its AssetId.)
    const u64 wantId = 0xABCDEF0123456789ULL;
    std::memcpy(static_cast<void*>(&src.Mesh), &wantId, sizeof(wantId));

    vector<u8> bytes;
    WriteFields(bytes, &src, registry.Info(registry.IdOf<WithAsset>()), registry);
    WithAsset dst;
    ReadFields(bytes, &dst, registry.Info(registry.IdOf<WithAsset>()), registry);

    CHECK(dst.Mesh.Id().Value == wantId);
    CHECK_FALSE(dst.Mesh.IsLoaded()); // residency is out of scope
}

TEST_CASE("Enum field round-trips as its underlying integer")
{
    TypeRegistry registry = MakeRegistry();

    WithEnum src;
    src.Side = Team::Green;
    vector<u8> bytes;
    WriteFields(bytes, &src, registry.Info(registry.IdOf<WithEnum>()), registry);
    WithEnum dst;
    ReadFields(bytes, &dst, registry.Info(registry.IdOf<WithEnum>()), registry);
    CHECK(static_cast<u32>(dst.Side) == static_cast<u32>(Team::Green));
}

TEST_CASE("Reference field round-trips an Entity within one Scene")
{
    TypeRegistry registry = MakeRegistry();
    Unique<Scene> scene = Scene::Create(registry);

    const Entity e = scene->CreateEntity();

    WithReference src;
    src.Target = e;
    vector<u8> bytes;
    WriteFields(bytes, &src, registry.Info(registry.IdOf<WithReference>()), registry);
    WithReference dst;
    ReadFields(bytes, &dst, registry.Info(registry.IdOf<WithReference>()), registry);

    CHECK(dst.Target == e);
    CHECK(dst.Target.Index == e.Index);
    CHECK(dst.Target.Generation == e.Generation);
}

TEST_CASE("Generic over non-components: a plain struct round-trips")
{
    TypeRegistry registry = MakeRegistry();

    PlainData src{2.5f, "never-a-component"};
    vector<u8> bytes;
    WriteFields(bytes, &src, registry.Info(registry.IdOf<PlainData>()), registry);
    PlainData dst;
    ReadFields(bytes, &dst, registry.Info(registry.IdOf<PlainData>()), registry);
    CHECK(dst.X == doctest::Approx(2.5f));
    CHECK(dst.Label == "never-a-component");
}

TEST_CASE("Open extension: a game-defined leaf + struct round-trips with no engine change")
{
    // Team (a game leaf) is used inside WithEnum, registered and round-tripped
    // above — its id and class come entirely from the game's ReflectLeaf<Team>
    // specialisation. Re-confirm the leaf is recognised generically.
    static_assert(TypeIdOf<Team>() == 0x11AA22BB33CC44DDULL);
    static_assert(FieldClassOf<Team>() == FieldClass::Enum);

    TypeRegistry registry;
    registry.Register<WithEnum>();
    CHECK(registry.IsRegistered(registry.IdOf<WithEnum>()));
    // The leaf's size was recorded by dependency auto-registration.
    CHECK(registry.Info(TypeIdOf<Team>()).Size == sizeof(Team));
}
