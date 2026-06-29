// Device-free cases for the shared material-codegen library: the schema-independent
// node catalog, the coercion-aware CanConnect predicate + CoerceExpr, and the
// topological emit walk (CompileMaterialGraph: graph → generated Slang source). No
// Context / Material / Vulkan symbol is touched.

#include <doctest/doctest.h>

#include <VengGraph/NodeGraph.h>
#include <VengGraph/NodeType.h>
#include <VengGraph/NodeGraphSerialize.h>
#include <VengGraph/MaterialCatalog.h>
#include <VengGraph/MaterialCompile.h>

#include <Veng/Reflection/TypeId.h>
#include <Veng/Asset/Material.h>

#include <cstring>
#include <string_view>

using namespace VengGraph;
using Veng::MaterialDomain;
using Veng::TypeIdOf;

namespace
{
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

    bool Contains(const Veng::string& haystack, std::string_view needle)
    {
        return haystack.find(needle) != Veng::string::npos;
    }
}

TEST_CASE("MaterialCatalog: the Surface MaterialOutput is the g-buffer contract")
{
    NodeCatalog catalog;
    MaterialEmitTable emit;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, emit, MaterialDomain::Surface);

    const NodeType* output = catalog.Find(types.MaterialOutput);
    REQUIRE(output != nullptr);

    // The Surface sinks express GBufferOutput: Albedo (vec4) + Normal (vec3).
    REQUIRE(output->Inputs.size() == 2);
    CHECK(output->Inputs[0].Name == OutputAlbedoPin);
    CHECK(output->Inputs[1].Name == OutputNormalPin);
    CHECK(output->Inputs[0].Type.Type == TypeIdOf<Veng::vec4>());
    CHECK(output->Inputs[1].Type.Type == TypeIdOf<Veng::vec3>());
}

TEST_CASE("MaterialCatalog: the PostProcess MaterialOutput is a single Color sink")
{
    NodeCatalog catalog;
    MaterialEmitTable emit;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, emit, MaterialDomain::PostProcess);

    const NodeType* output = catalog.Find(types.MaterialOutput);
    REQUIRE(output != nullptr);

    REQUIRE(output->Inputs.size() == 1);
    CHECK(output->Inputs[0].Name == OutputColorPin);
    CHECK(output->Inputs[0].Type.Type == TypeIdOf<Veng::vec4>());
}

TEST_CASE("MaterialCatalog: the node set is schema-independent")
{
    // Two registrations of the same domain yield the same fixed node shapes — the
    // catalog is a fixed function of the domain, not of any loaded shader's fields.
    NodeCatalog catalog;
    MaterialEmitTable emit;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, emit, MaterialDomain::Surface);

    const NodeType* sample = catalog.Find(types.TextureSample);
    REQUIRE(sample != nullptr);
    REQUIRE(sample->Inputs.size() == 1);
    CHECK(sample->Inputs[0].Name == TextureSampleUVPin);
    REQUIRE(sample->Outputs.size() == 1);
    CHECK(sample->Outputs[0].Type.Type == TypeIdOf<Veng::vec4>());

    const NodeType* param = catalog.Find(types.Param);
    REQUIRE(param != nullptr);
    REQUIRE(param->Outputs.size() == 1);
    CHECK(param->Outputs[0].Type.Type == TypeIdOf<Veng::vec4>());

    // The emit table carries an emit-fn for the two value-producing types.
    CHECK(emit.Find(types.TextureSample) != nullptr);
    CHECK(emit.Find(types.Param) != nullptr);
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

TEST_CASE("MaterialCatalog: CanConnect enforces exact-type + the coercions")
{
    const PinType f32{.Kind = PinType::Kind::Value, .Type = TypeIdOf<Veng::f32>()};
    const PinType vec2{.Kind = PinType::Kind::Value, .Type = TypeIdOf<Veng::vec2>()};
    const PinType vec3{.Kind = PinType::Kind::Value, .Type = TypeIdOf<Veng::vec3>()};
    const PinType vec4{.Kind = PinType::Kind::Value, .Type = TypeIdOf<Veng::vec4>()};

    CHECK(MaterialCanConnect(vec4, vec4));
    CHECK(MaterialCanConnect(f32, f32));

    CHECK(MaterialCanConnect(f32, vec2));
    CHECK(MaterialCanConnect(f32, vec3));
    CHECK(MaterialCanConnect(f32, vec4));

    CHECK(MaterialCanConnect(vec4, vec3));
    CHECK(MaterialCanConnect(vec4, vec2));

    CHECK_FALSE(MaterialCanConnect(vec2, vec4));
    CHECK_FALSE(MaterialCanConnect(vec4, f32));
}

TEST_CASE("MaterialCatalog: CoerceExpr wraps by type")
{
    const PinType f32{.Kind = PinType::Kind::Value, .Type = TypeIdOf<Veng::f32>()};
    const PinType vec2{.Kind = PinType::Kind::Value, .Type = TypeIdOf<Veng::vec2>()};
    const PinType vec3{.Kind = PinType::Kind::Value, .Type = TypeIdOf<Veng::vec3>()};
    const PinType vec4{.Kind = PinType::Kind::Value, .Type = TypeIdOf<Veng::vec4>()};

    // Identity passes through unchanged.
    CHECK(CoerceExpr("x", vec4, vec4) == "x");

    // f32 → vecN splats.
    CHECK(CoerceExpr("x", f32, vec3) == "(x).xxx");
    CHECK(CoerceExpr("x", f32, vec4) == "(x).xxxx");

    // vec4 → lower-arity truncates.
    CHECK(CoerceExpr("x", vec4, vec3) == "(x).xyz");
    CHECK(CoerceExpr("x", vec4, vec2) == "(x).xy");
}

TEST_CASE("CompileMaterialGraph: error without a single MaterialOutput")
{
    NodeCatalog catalog;
    MaterialEmitTable emit;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, emit, MaterialDomain::Surface);

    // No MaterialOutput → error.
    {
        NodeGraph graph = MakeGraph(catalog);
        graph.AddNode(types.Param);
        const Veng::Result<GeneratedFragment> r =
            CompileMaterialGraph(graph, catalog, emit, MaterialDomain::Surface);
        CHECK_FALSE(r.has_value());
    }

    // Two MaterialOutputs → error.
    {
        NodeGraph graph = MakeGraph(catalog);
        graph.AddNode(types.MaterialOutput);
        graph.AddNode(types.MaterialOutput);
        const Veng::Result<GeneratedFragment> r =
            CompileMaterialGraph(graph, catalog, emit, MaterialDomain::Surface);
        CHECK_FALSE(r.has_value());
    }
}

TEST_CASE("CompileMaterialGraph: a bare Surface output emits defined defaults")
{
    NodeCatalog catalog;
    MaterialEmitTable emit;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, emit, MaterialDomain::Surface);

    NodeGraph graph = MakeGraph(catalog);
    graph.AddNode(types.MaterialOutput);

    const Veng::Result<GeneratedFragment> r =
        CompileMaterialGraph(graph, catalog, emit, MaterialDomain::Surface);
    REQUIRE(r.has_value());
    const Veng::string& src = r->Source;

    CHECK(Contains(src, "#include \"Veng/material.slang\""));
    CHECK(Contains(src, "GBufferOutput fsMain(SurfaceFragmentInput input)"));
    // Unconnected sink defaults.
    CHECK(Contains(src, "o.Albedo = float4(0,0,0,1)"));
    CHECK(Contains(src, "input.v_WorldNormal"));
    CHECK(Contains(src, "o.ORM = float4(1, 1, 0, 0)"));
    // Velocity is always written, never authorable away.
    CHECK(Contains(src, "o.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip)"));
}

TEST_CASE("CompileMaterialGraph: a bare PostProcess output passes the screen sample")
{
    NodeCatalog catalog;
    MaterialEmitTable emit;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, emit, MaterialDomain::PostProcess);

    NodeGraph graph = MakeGraph(catalog);
    graph.AddNode(types.MaterialOutput);

    const Veng::Result<GeneratedFragment> r =
        CompileMaterialGraph(graph, catalog, emit, MaterialDomain::PostProcess);
    REQUIRE(r.has_value());
    const Veng::string& src = r->Source;

    CHECK(Contains(src, "float4 fsMain(PostProcessFragmentInput input) : SV_Target0"));
    CHECK(Contains(src, "g_PC.MaterialIndex"));
}

TEST_CASE("CompileMaterialGraph: a connected Param feeds the Albedo sink")
{
    NodeCatalog catalog;
    MaterialEmitTable emit;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, emit, MaterialDomain::Surface);

    NodeGraph graph = MakeGraph(catalog);
    const NodeId output = graph.AddNode(types.MaterialOutput);
    const NodeId param = graph.AddNode(types.Param);

    // Param (vec4) → Albedo (vec4): exact type, no coercion.
    const Veng::VoidResult c =
        graph.Connect(PinRef{.Node = param, .Pin = 0}, PinRef{.Node = output, .Pin = 0});
    REQUIRE(c.has_value());

    const Veng::Result<GeneratedFragment> r =
        CompileMaterialGraph(graph, catalog, emit, MaterialDomain::Surface);
    REQUIRE(r.has_value());
    const Veng::string& src = r->Source;

    // The single-use Param value inlines into the Albedo write (no temp).
    CHECK(Contains(src, "o.Albedo = p."));
    CHECK_FALSE(Contains(src, "o.Albedo = float4(0,0,0,1)"));
}

TEST_CASE("CompileMaterialGraph: coercion is applied at a lower-arity sink")
{
    NodeCatalog catalog;
    MaterialEmitTable emit;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, emit, MaterialDomain::Surface);

    NodeGraph graph = MakeGraph(catalog);
    const NodeId output = graph.AddNode(types.MaterialOutput);
    const NodeId sample = graph.AddNode(types.TextureSample);

    // TextureSample (vec4) → Normal (vec3): truncating coercion.
    const Veng::VoidResult c =
        graph.Connect(PinRef{.Node = sample, .Pin = 0}, PinRef{.Node = output, .Pin = 1});
    REQUIRE(c.has_value());

    const Veng::Result<GeneratedFragment> r =
        CompileMaterialGraph(graph, catalog, emit, MaterialDomain::Surface);
    REQUIRE(r.has_value());
    // The vec4 sample expression is truncated to vec3 before the Normal write.
    CHECK(Contains(r->Source, ".xyz, 0)"));
}

TEST_CASE("CompileMaterialGraph: a shared TextureSample emits one temp, sampled once")
{
    NodeCatalog catalog;
    MaterialEmitTable emit;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, emit, MaterialDomain::Surface);

    NodeGraph graph = MakeGraph(catalog);
    const NodeId output = graph.AddNode(types.MaterialOutput);
    const NodeId sample = graph.AddNode(types.TextureSample);

    // The sample feeds both Albedo (vec4) and Normal (vec3): used twice → a temp.
    REQUIRE(graph.Connect(PinRef{.Node = sample, .Pin = 0}, PinRef{.Node = output, .Pin = 0})
                .has_value());
    REQUIRE(graph.Connect(PinRef{.Node = sample, .Pin = 0}, PinRef{.Node = output, .Pin = 1})
                .has_value());

    const Veng::Result<GeneratedFragment> r =
        CompileMaterialGraph(graph, catalog, emit, MaterialDomain::Surface);
    REQUIRE(r.has_value());
    const Veng::string& src = r->Source;

    // The Sample(...) call appears exactly once (in the temp declaration).
    Veng::usize count = 0;
    Veng::usize pos = 0;
    while ((pos = src.find(".Sample(", pos)) != Veng::string::npos)
    {
        ++count;
        pos += 1;
    }
    CHECK(count == 1);
    // The temp is declared with a float4 type and the node-keyed Color name.
    CHECK(Contains(src, "float4 n"));
}

TEST_CASE("CompileMaterialGraph: an unreached node never emits (dead-code elimination)")
{
    NodeCatalog catalog;
    MaterialEmitTable emit;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, emit, MaterialDomain::Surface);

    NodeGraph graph = MakeGraph(catalog);
    graph.AddNode(types.MaterialOutput);
    graph.AddNode(types.TextureSample); // unconnected: never reached

    const Veng::Result<GeneratedFragment> r =
        CompileMaterialGraph(graph, catalog, emit, MaterialDomain::Surface);
    REQUIRE(r.has_value());
    // No sample emitted from the orphan node.
    CHECK_FALSE(Contains(r->Source, ".Sample("));
}

TEST_CASE("CompileMaterialGraph: the output is deterministic across walks and round-trip")
{
    NodeCatalog catalog;
    MaterialEmitTable emit;
    const MaterialNodeTypes types =
        RegisterMaterialNodeTypes(catalog, emit, MaterialDomain::Surface);

    NodeGraph graph = MakeGraph(catalog);
    const NodeId output = graph.AddNode(types.MaterialOutput);
    const NodeId sample = graph.AddNode(types.TextureSample);
    const NodeId param = graph.AddNode(types.Param);
    REQUIRE(graph.Connect(PinRef{.Node = sample, .Pin = 0}, PinRef{.Node = output, .Pin = 0})
                .has_value());
    REQUIRE(graph.Connect(PinRef{.Node = param, .Pin = 0}, PinRef{.Node = output, .Pin = 1})
                .has_value());

    const Veng::Result<GeneratedFragment> a =
        CompileMaterialGraph(graph, catalog, emit, MaterialDomain::Surface);
    const Veng::Result<GeneratedFragment> b =
        CompileMaterialGraph(graph, catalog, emit, MaterialDomain::Surface);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    // Two walks of the same graph emit byte-identical text.
    CHECK(a->Source == b->Source);

    // A save/load round-trip preserves creation order, so the keys (and the text) match.
    const Veng::string doc = WriteNodeGraph(graph, catalog);
    NodeGraph reloaded = MakeGraph(catalog);
    const NodeGraphReadOutcome outcome = ReadNodeGraph(doc, reloaded, catalog);
    REQUIRE(outcome == NodeGraphReadOutcome::Loaded);

    const Veng::Result<GeneratedFragment> c =
        CompileMaterialGraph(reloaded, catalog, emit, MaterialDomain::Surface);
    REQUIRE(c.has_value());
    CHECK(c->Source == a->Source);
}
