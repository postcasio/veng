// Layer-3 device-free cases: the material node catalog, the coercion-aware
// CanConnect predicate, CompileMaterialGraph (graph → typed .vmat field list),
// and BuildGraphFromMaterial (flat .vmat field table → default graph). The
// MaterialShaderInterface is built from a hand-authored vector<MaterialField>,
// so no Context / Material / Vulkan symbol is touched.

#include <doctest/doctest.h>

#include "material/MaterialCatalog.h"
#include "material/MaterialCompile.h"
#include "material/MaterialShaderInterface.h"

#include <VengEditor/NodeGraph/NodeGraph.h>
#include <VengEditor/NodeGraph/NodeType.h>

#include <Veng/Reflection/TypeId.h>
#include <Veng/Asset/Material.h>

#include <algorithm>
#include <cstring>

using namespace VengEditor;
using Veng::TypeIdOf;

namespace
{
    using Field = Veng::MaterialField;
    using Kind = Veng::MaterialField::FieldKind;

    // The brick material's reflected field table: a texture + its paired sampler
    // + a vec4 param. Mirrors brick.vmat.json's three fields after cook.
    Veng::vector<Field> BrickFields()
    {
        return {
            Field{.Name = "Albedo", .Offset = 0, .Size = 4, .Kind = Kind::TextureHandle, .TextureId = 1001},
            Field{.Name = "AlbedoSampler", .Offset = 4, .Size = 4, .Kind = Kind::SamplerHandle, .TextureId = 1001},
            Field{.Name = "Factors", .Offset = 0, .Size = 16, .Kind = Kind::Param},
        };
    }

    NodeGraph MakeGraph(const NodeCatalog& catalog)
    {
        return NodeGraph(
            MaterialCanConnect,
            [&catalog](NodeTypeId id) { return catalog.ShapeOf(id); },
            [&catalog](NodeTypeId id) {
                const NodeType* type = catalog.Find(id);
                return type ? type->PropertySize : Veng::usize{0};
            });
    }

    const CompiledField* FindField(const Veng::vector<CompiledField>& fields, Veng::string_view name)
    {
        for (const CompiledField& f : fields)
            if (f.Name == name)
                return &f;
        return nullptr;
    }
}

TEST_CASE("MaterialCatalog: MaterialOutput pins derive from the field table by Kind")
{
    const Veng::vector<Field> brick = BrickFields();
    const MaterialShaderInterface iface{.Fields = brick};

    NodeCatalog catalog;
    const MaterialNodeTypes types = RegisterMaterialNodeTypes(catalog, iface);

    const NodeType* output = catalog.Find(types.MaterialOutput);
    REQUIRE(output != nullptr);

    // A pin for the texture field and the param field; the sampler field gets none.
    REQUIRE(output->Inputs.size() == 2);
    CHECK(output->Inputs[0].Name == "Albedo");
    CHECK(output->Inputs[1].Name == "Factors");

    // The texture pin is vec4 (a Color), the param pin is the field's arity.
    CHECK(output->Inputs[0].Type.Type == TypeIdOf<Veng::vec4>());
    CHECK(output->Inputs[1].Type.Type == TypeIdOf<Veng::vec4>());
}

TEST_CASE("MaterialCatalog: CanConnect enforces exact-type + the v1 coercions")
{
    const PinType f32{PinType::Kind::Value, TypeIdOf<Veng::f32>()};
    const PinType vec2{PinType::Kind::Value, TypeIdOf<Veng::vec2>()};
    const PinType vec3{PinType::Kind::Value, TypeIdOf<Veng::vec3>()};
    const PinType vec4{PinType::Kind::Value, TypeIdOf<Veng::vec4>()};

    // Exact identity.
    CHECK(MaterialCanConnect(vec4, vec4));
    CHECK(MaterialCanConnect(f32, f32));

    // f32 → vecN splat.
    CHECK(MaterialCanConnect(f32, vec2));
    CHECK(MaterialCanConnect(f32, vec3));
    CHECK(MaterialCanConnect(f32, vec4));

    // vec4 → vec3 / vec2 truncate.
    CHECK(MaterialCanConnect(vec4, vec3));
    CHECK(MaterialCanConnect(vec4, vec2));

    // No widening: vec2 → vec4 is rejected.
    CHECK_FALSE(MaterialCanConnect(vec2, vec4));
    // No truncation onto a scalar field via a texture color.
    CHECK_FALSE(MaterialCanConnect(vec4, f32));
}

TEST_CASE("MaterialCompile: BuildGraphFromMaterial → CompileMaterialGraph is field-stable")
{
    const Veng::vector<Field> brick = BrickFields();
    const MaterialShaderInterface iface{
        .Fields = brick,
        .VertexShader = Veng::AssetId{1004},
        .FragmentShader = Veng::AssetId{1005},
    };

    NodeCatalog catalog;
    const MaterialNodeTypes types = RegisterMaterialNodeTypes(catalog, iface);

    const NodeGraph graph = BuildGraphFromMaterial(iface, catalog, types);

    // The synthesized graph: MaterialOutput + a TextureSample (Albedo) + a Param
    // (Factors). The sampler field is consumed by its texture, never its own node.
    CHECK(graph.Nodes().size() == 3);
    CHECK(graph.Links().size() == 2);

    const Veng::Result<Veng::vector<CompiledField>> compiled =
        CompileMaterialGraph(graph, catalog, iface);
    REQUIRE(compiled.has_value());

    // The field list reproduces the source structure: texture + paired sampler +
    // the param, with the texture's id carried through the TextureSample property.
    const Veng::vector<CompiledField>& fields = *compiled;
    REQUIRE(fields.size() == 3);

    const CompiledField* albedo = FindField(fields, "Albedo");
    REQUIRE(albedo != nullptr);
    CHECK(albedo->Type == "texture");
    CHECK(albedo->TextureId == 1001ULL);

    const CompiledField* sampler = FindField(fields, "AlbedoSampler");
    REQUIRE(sampler != nullptr);
    CHECK(sampler->Type == "sampler");
    CHECK(sampler->SamplerTexture == "Albedo");

    const CompiledField* factors = FindField(fields, "Factors");
    REQUIRE(factors != nullptr);
    CHECK(factors->Type == "vec4");
    CHECK(factors->Values.size() == 4);
}

TEST_CASE("MaterialCompile: a Param edit flows through compile")
{
    const Veng::vector<Field> brick = BrickFields();
    const MaterialShaderInterface iface{.Fields = brick};

    NodeCatalog catalog;
    const MaterialNodeTypes types = RegisterMaterialNodeTypes(catalog, iface);

    NodeGraph graph = MakeGraph(catalog);
    const NodeId output = graph.AddNode(types.MaterialOutput);
    const NodeId paramNode = graph.AddNode(types.Param);

    // Set the Param's value before wiring it into the Factors pin (pin index 1).
    const NodeType* paramType = catalog.Find(types.Param);
    REQUIRE(paramType != nullptr);
    const Veng::vec4 value{0.25f, 0.5f, 0.75f, 1.0f};
    const std::byte* valueBytes = reinterpret_cast<const std::byte*>(&value);
    graph.SetProperty(paramNode, paramType->Properties[0],
                      std::span<const std::byte>(valueBytes, sizeof(value)));

    REQUIRE(graph.Connect(PinRef{paramNode, 0}, PinRef{output, 1}).has_value());

    const Veng::Result<Veng::vector<CompiledField>> compiled =
        CompileMaterialGraph(graph, catalog, iface);
    REQUIRE(compiled.has_value());

    const CompiledField* factors = FindField(*compiled, "Factors");
    REQUIRE(factors != nullptr);
    REQUIRE(factors->Values.size() == 4);
    CHECK(factors->Values[0] == doctest::Approx(0.25f));
    CHECK(factors->Values[3] == doctest::Approx(1.0f));
}

TEST_CASE("MaterialCompile: an f32 Param into a vec4 pin compiles to a splat")
{
    const Veng::vector<Field> brick = BrickFields();
    const MaterialShaderInterface iface{.Fields = brick};

    NodeCatalog catalog;
    const MaterialNodeTypes types = RegisterMaterialNodeTypes(catalog, iface);

    // A scalar-output param feeding the vec4 Factors pin exercises the f32→vec4
    // splat the coercion predicate permits and the compiler applies.
    struct ScalarProps { Veng::vec4 Value{0.0f, 0.0f, 0.0f, 0.0f}; };
    const NodeTypeId scalarParam = catalog.Register(NodeType{
        .Name = "ScalarParam",
        .Outputs = {PinDesc{"Value", PinType{PinType::Kind::Value, TypeIdOf<Veng::f32>()}}},
        .Properties = {Veng::FieldDescriptor{
            .Name = "Value",
            .Type = TypeIdOf<Veng::vec4>(),
            .Class = Veng::FieldClass::Vector,
            .Offset = offsetof(ScalarProps, Value),
        }},
        .PropertySize = sizeof(ScalarProps),
    });

    NodeGraph graph = MakeGraph(catalog);
    const NodeId output = graph.AddNode(types.MaterialOutput);
    const NodeId scalar = graph.AddNode(scalarParam);

    const NodeType* scalarType = catalog.Find(scalarParam);
    const Veng::vec4 value{0.7f, 0.0f, 0.0f, 0.0f};
    const std::byte* valueBytes = reinterpret_cast<const std::byte*>(&value);
    graph.SetProperty(scalar, scalarType->Properties[0],
                      std::span<const std::byte>(valueBytes, sizeof(value)));

    REQUIRE(graph.Connect(PinRef{scalar, 0}, PinRef{output, 1}).has_value());

    const Veng::Result<Veng::vector<CompiledField>> compiled =
        CompileMaterialGraph(graph, catalog, iface);
    REQUIRE(compiled.has_value());

    const CompiledField* factors = FindField(*compiled, "Factors");
    REQUIRE(factors != nullptr);
    CHECK(factors->Type == "vec4");
    REQUIRE(factors->Values.size() == 4);
    for (Veng::f32 v : factors->Values)
        CHECK(v == doctest::Approx(0.7f));
}

TEST_CASE("MaterialCompile: an unconnected output field is omitted")
{
    const Veng::vector<Field> brick = BrickFields();
    const MaterialShaderInterface iface{.Fields = brick};

    NodeCatalog catalog;
    const MaterialNodeTypes types = RegisterMaterialNodeTypes(catalog, iface);

    // Only the MaterialOutput node; nothing wired in.
    NodeGraph graph = MakeGraph(catalog);
    graph.AddNode(types.MaterialOutput);

    const Veng::Result<Veng::vector<CompiledField>> compiled =
        CompileMaterialGraph(graph, catalog, iface);
    REQUIRE(compiled.has_value());
    CHECK(compiled->empty());
}

TEST_CASE("MaterialCompile: an incompatible connection is rejected by CanConnect")
{
    const Veng::vector<Field> brick = BrickFields();
    const MaterialShaderInterface iface{.Fields = brick};

    NodeCatalog catalog;
    const MaterialNodeTypes types = RegisterMaterialNodeTypes(catalog, iface);

    // A texture-only field table whose pin is vec4 (Color), plus a vec2-only
    // source: connecting vec2 → vec4 must be rejected (no widening).
    NodeGraph graph = MakeGraph(catalog);
    const NodeId output = graph.AddNode(types.MaterialOutput);

    const NodeTypeId vec2Source = catalog.Register(NodeType{
        .Name = "Vec2Source",
        .Outputs = {PinDesc{"Out", PinType{PinType::Kind::Value, TypeIdOf<Veng::vec2>()}}},
    });
    const NodeId source = graph.AddNode(vec2Source);

    // Pin 0 (Albedo) is vec4; vec2 → vec4 is not a permitted coercion.
    const Veng::VoidResult result = graph.Connect(PinRef{source, 0}, PinRef{output, 0});
    CHECK_FALSE(result.has_value());
}
