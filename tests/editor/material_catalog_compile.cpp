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
using Veng::MaterialDomain;
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
            Field{.Name = "Albedo",
                  .Offset = 0,
                  .Size = 4,
                  .Kind = Kind::TextureHandle,
                  .TextureId = 1001},
            Field{.Name = "AlbedoSampler",
                  .Offset = 4,
                  .Size = 4,
                  .Kind = Kind::SamplerHandle,
                  .TextureId = 1001},
            Field{.Name = "Factors", .Offset = 0, .Size = 16, .Kind = Kind::Param},
        };
    }

    // A postprocess material's reflected field table: a runtime-bound input handle
    // + its sampler + an exposed scalar param. Mirrors a tonemap.vmat after cook.
    Veng::vector<Field> TonemapFields()
    {
        return {
            Field{
                .Name = "Hdr", .Offset = 0, .Size = 4, .Kind = Kind::TextureHandle, .TextureId = 0},
            Field{.Name = "HdrSampler",
                  .Offset = 4,
                  .Size = 4,
                  .Kind = Kind::SamplerHandle,
                  .TextureId = 0},
            Field{.Name = "Exposure", .Offset = 0, .Size = 4, .Kind = Kind::Param},
        };
    }

    NodeGraph MakeGraph(const NodeCatalog& catalog)
    {
        return NodeGraph(
            MaterialCanConnect, [&catalog](NodeTypeId id) { return catalog.ShapeOf(id); },
            [&catalog](NodeTypeId id)
            {
                const NodeType* type = catalog.Find(id);
                return type ? type->PropertySize : Veng::usize{0};
            });
    }

    const CompiledField* FindField(const Veng::vector<CompiledField>& fields,
                                   Veng::string_view name)
    {
        for (const CompiledField& f : fields)
        {
            if (f.Name == name)
            {
                return &f;
            }
        }
        return nullptr;
    }
}

TEST_CASE("MaterialCatalog: the Surface MaterialOutput is the g-buffer contract")
{
    const Veng::vector<Field> brick = BrickFields();
    const MaterialShaderInterface iface{.Fields = brick};

    NodeCatalog catalog;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, iface, MaterialDomain::Surface);

    const NodeType* output = catalog.Find(types.MaterialOutput);
    REQUIRE(output != nullptr);

    // The Surface sinks express GBufferOutput: Albedo (vec4) + Normal (vec3),
    // independent of the loaded shader's reflected fields.
    REQUIRE(output->Inputs.size() == 2);
    CHECK(output->Inputs[0].Name == OutputAlbedoPin);
    CHECK(output->Inputs[1].Name == OutputNormalPin);
    CHECK(output->Inputs[0].Type.Type == TypeIdOf<Veng::vec4>());
    CHECK(output->Inputs[1].Type.Type == TypeIdOf<Veng::vec3>());
}

TEST_CASE("MaterialCatalog: the PostProcess MaterialOutput is a single Color sink")
{
    const Veng::vector<Field> tonemap = TonemapFields();
    const MaterialShaderInterface iface{.Fields = tonemap};

    NodeCatalog catalog;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, iface, MaterialDomain::PostProcess);

    const NodeType* output = catalog.Find(types.MaterialOutput);
    REQUIRE(output != nullptr);

    // PostProcess authors one output: the final color.
    REQUIRE(output->Inputs.size() == 1);
    CHECK(output->Inputs[0].Name == OutputColorPin);
    CHECK(output->Inputs[0].Type.Type == TypeIdOf<Veng::vec4>());
}

TEST_CASE("MaterialCatalog: DomainOutputContract is the per-domain sink table")
{
    const Veng::vector<DomainOutputPin> surface = DomainOutputContract(MaterialDomain::Surface);
    REQUIRE(surface.size() == 2);
    CHECK(surface[0].Name == OutputAlbedoPin);
    CHECK(surface[0].Type.Type == TypeIdOf<Veng::vec4>());
    CHECK(surface[1].Name == OutputNormalPin);
    CHECK(surface[1].Type.Type == TypeIdOf<Veng::vec3>());

    const Veng::vector<DomainOutputPin> post = DomainOutputContract(MaterialDomain::PostProcess);
    REQUIRE(post.size() == 1);
    CHECK(post[0].Name == OutputColorPin);
    CHECK(post[0].Type.Type == TypeIdOf<Veng::vec4>());
}

TEST_CASE("MaterialCatalog: CanConnect enforces exact-type + the v1 coercions")
{
    const PinType f32{.Kind = PinType::Kind::Value, .Type = TypeIdOf<Veng::f32>()};
    const PinType vec2{.Kind = PinType::Kind::Value, .Type = TypeIdOf<Veng::vec2>()};
    const PinType vec3{.Kind = PinType::Kind::Value, .Type = TypeIdOf<Veng::vec3>()};
    const PinType vec4{.Kind = PinType::Kind::Value, .Type = TypeIdOf<Veng::vec4>()};

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

TEST_CASE("MaterialCompile: a Surface BuildGraphFromMaterial → Compile is field-stable")
{
    const Veng::vector<Field> brick = BrickFields();
    const MaterialShaderInterface iface{
        .Fields = brick,
        .VertexShader = Veng::AssetId{1004},
        .FragmentShader = Veng::AssetId{1005},
    };

    NodeCatalog catalog;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, iface, MaterialDomain::Surface);

    const NodeGraph graph = BuildGraphFromMaterial(iface, catalog, types);

    // The synthesized graph: MaterialOutput + a TextureSample (Albedo) + a Param
    // (Factors). The sampler field is consumed by its texture, never its own node.
    CHECK(graph.Nodes().size() == 3);

    const Veng::Result<Veng::vector<CompiledField>> compiled =
        CompileMaterialGraph(graph, catalog, iface, MaterialDomain::Surface);
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

TEST_CASE("MaterialCompile: a PostProcess BuildGraphFromMaterial → Compile is field-stable")
{
    const Veng::vector<Field> tonemap = TonemapFields();
    const MaterialShaderInterface iface{
        .Fields = tonemap,
        .VertexShader = Veng::AssetId{2004},
        .FragmentShader = Veng::AssetId{2005},
    };

    NodeCatalog catalog;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, iface, MaterialDomain::PostProcess);

    const NodeGraph graph = BuildGraphFromMaterial(iface, catalog, types);

    // The synthesized graph: MaterialOutput + a TextureSample (Hdr) + a Param
    // (Exposure). The single Color sink does not name the bound fields; they come
    // from the upstream feeders.
    CHECK(graph.Nodes().size() == 3);

    const Veng::Result<Veng::vector<CompiledField>> compiled =
        CompileMaterialGraph(graph, catalog, iface, MaterialDomain::PostProcess);
    REQUIRE(compiled.has_value());

    const Veng::vector<CompiledField>& fields = *compiled;
    REQUIRE(fields.size() == 3);

    const CompiledField* hdr = FindField(fields, "Hdr");
    REQUIRE(hdr != nullptr);
    CHECK(hdr->Type == "texture");

    const CompiledField* sampler = FindField(fields, "HdrSampler");
    REQUIRE(sampler != nullptr);
    CHECK(sampler->Type == "sampler");
    CHECK(sampler->SamplerTexture == "Hdr");

    const CompiledField* exposure = FindField(fields, "Exposure");
    REQUIRE(exposure != nullptr);
    CHECK(exposure->Type == "float");
    REQUIRE(exposure->Values.size() == 1);
}

TEST_CASE("MaterialCompile: a Param edit flows through compile")
{
    const Veng::vector<Field> brick = BrickFields();
    const MaterialShaderInterface iface{.Fields = brick};

    NodeCatalog catalog;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, iface, MaterialDomain::Surface);

    // Build the default graph, then set the Param feeder's value. Compile sources
    // the Factors field from the Param feeder, independent of the sink wiring.
    NodeGraph graph = BuildGraphFromMaterial(iface, catalog, types);

    NodeId paramNode{};
    for (const NodeId node : graph.Nodes())
    {
        if (graph.GetTypeOf(node) == types.Param)
        {
            paramNode = node;
        }
    }
    REQUIRE(graph.IsValid(paramNode));

    const NodeType* paramType = catalog.Find(types.Param);
    REQUIRE(paramType != nullptr);
    const Veng::vec4 value{0.25f, 0.5f, 0.75f, 1.0f};
    const auto* valueBytes = reinterpret_cast<const std::byte*>(&value);
    graph.SetProperty(paramNode, paramType->Properties[0],
                      std::span<const std::byte>(valueBytes, sizeof(value)));

    const Veng::Result<Veng::vector<CompiledField>> compiled =
        CompileMaterialGraph(graph, catalog, iface, MaterialDomain::Surface);
    REQUIRE(compiled.has_value());

    const CompiledField* factors = FindField(*compiled, "Factors");
    REQUIRE(factors != nullptr);
    REQUIRE(factors->Values.size() == 4);
    CHECK(factors->Values[0] == doctest::Approx(0.25f));
    CHECK(factors->Values[3] == doctest::Approx(1.0f));
}

TEST_CASE("MaterialCompile: a param feeder's value coerces to its field arity")
{
    // A scalar-sized param field reads only the leading component of the feeding
    // Param's vec4 value — the field's arity drives the emitted JSON arity.
    const Veng::vector<Field> scalarOnly = {
        Field{.Name = "Intensity", .Offset = 0, .Size = 4, .Kind = Kind::Param},
    };
    const MaterialShaderInterface iface{.Fields = scalarOnly};

    NodeCatalog catalog;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, iface, MaterialDomain::PostProcess);

    NodeGraph graph = BuildGraphFromMaterial(iface, catalog, types);

    NodeId paramNode{};
    for (const NodeId node : graph.Nodes())
    {
        if (graph.GetTypeOf(node) == types.Param)
        {
            paramNode = node;
        }
    }
    REQUIRE(graph.IsValid(paramNode));

    const NodeType* paramType = catalog.Find(types.Param);
    const Veng::vec4 value{0.7f, 0.1f, 0.2f, 0.3f};
    const auto* valueBytes = reinterpret_cast<const std::byte*>(&value);
    graph.SetProperty(paramNode, paramType->Properties[0],
                      std::span<const std::byte>(valueBytes, sizeof(value)));

    const Veng::Result<Veng::vector<CompiledField>> compiled =
        CompileMaterialGraph(graph, catalog, iface, MaterialDomain::PostProcess);
    REQUIRE(compiled.has_value());

    const CompiledField* intensity = FindField(*compiled, "Intensity");
    REQUIRE(intensity != nullptr);
    CHECK(intensity->Type == "float");
    REQUIRE(intensity->Values.size() == 1);
    CHECK(intensity->Values[0] == doctest::Approx(0.7f));
}

TEST_CASE("MaterialCompile: a feederless graph emits default-valued fields")
{
    const Veng::vector<Field> brick = BrickFields();
    const MaterialShaderInterface iface{.Fields = brick};

    NodeCatalog catalog;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, iface, MaterialDomain::Surface);

    // Only the MaterialOutput node; no feeders. The bound field set still mirrors
    // the loaded material's fields (zeroed values / ids), so the importer's schema
    // tolerance keeps the cooked defaults.
    NodeGraph graph = MakeGraph(catalog);
    graph.AddNode(types.MaterialOutput);

    const Veng::Result<Veng::vector<CompiledField>> compiled =
        CompileMaterialGraph(graph, catalog, iface, MaterialDomain::Surface);
    REQUIRE(compiled.has_value());
    REQUIRE(compiled->size() == 3);

    const CompiledField* albedo = FindField(*compiled, "Albedo");
    REQUIRE(albedo != nullptr);
    CHECK(albedo->Type == "texture");
    CHECK(albedo->TextureId == 0ULL);
}

TEST_CASE("MaterialCompile: an incompatible connection is rejected by CanConnect")
{
    const Veng::vector<Field> brick = BrickFields();
    const MaterialShaderInterface iface{.Fields = brick};

    NodeCatalog catalog;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, iface, MaterialDomain::Surface);

    // Pin 0 (Albedo) is vec4; a vec2-only source connecting vec2 → vec4 must be
    // rejected (no widening).
    NodeGraph graph = MakeGraph(catalog);
    const NodeId output = graph.AddNode(types.MaterialOutput);

    const NodeTypeId vec2Source = catalog.Register(NodeType{
        .Name = "Vec2Source",
        .Outputs = {PinDesc{
            .Name = "Out",
            .Type = PinType{.Kind = PinType::Kind::Value, .Type = TypeIdOf<Veng::vec2>()}}},
    });
    const NodeId source = graph.AddNode(vec2Source);

    const Veng::VoidResult result =
        graph.Connect(PinRef{.Node = source, .Pin = 0}, PinRef{.Node = output, .Pin = 0});
    CHECK_FALSE(result.has_value());
}
