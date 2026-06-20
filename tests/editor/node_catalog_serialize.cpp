// Layer-2 unit cases: the data-driven node catalog (Register/Find by id and
// name) and graph (de)serialization through the per-FieldClass JSON walker —
// property round-trip (vec4 + AssetHandle), tolerant drop of an unknown node
// type, and the version-too-new read-only signal. Device-free; no Context, no
// Vulkan symbol touched.

#include <doctest/doctest.h>

#include <VengEditor/NodeGraph/NodeGraph.h>
#include <VengEditor/NodeGraph/NodeType.h>
#include <VengEditor/NodeGraph/NodeGraphSerialize.h>

#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/Texture.h>

#include <nlohmann/json.hpp>

#include <cstring>

using namespace VengEditor;
using Veng::TypeIdOf;

namespace
{
    PinType ValueOf(Veng::TypeId id)
    {
        return PinType{PinType::Kind::Value, id};
    }

    // A node type carrying one vec4 property "Value".
    struct ValueProps
    {
        Veng::vec4 Value{0.0f, 0.0f, 0.0f, 0.0f};
    };

    // A node type carrying one AssetHandle<Texture> property "Texture".
    struct TextureProps
    {
        Veng::AssetHandle<Veng::Texture> Texture;
    };

    Veng::FieldDescriptor ValueField()
    {
        return Veng::FieldDescriptor{
            .Name = "Value",
            .Type = TypeIdOf<Veng::vec4>(),
            .Class = Veng::FieldClass::Vector,
            .Offset = offsetof(ValueProps, Value),
        };
    }

    Veng::FieldDescriptor TextureField()
    {
        return Veng::FieldDescriptor{
            .Name = "Texture",
            .Type = TypeIdOf<Veng::AssetHandle<Veng::Texture>>(),
            .Class = Veng::FieldClass::AssetHandle,
            .Offset = offsetof(TextureProps, Texture),
        };
    }

    // A catalog with: an output-only "Source" (one f32 out), a "Param" carrying a
    // vec4 property + one f32 out, a "Sample" carrying an AssetHandle property +
    // one f32 out, and an input-only "Output" (one f32 in).
    struct Fixture
    {
        NodeCatalog Catalog;
        NodeTypeId Source;
        NodeTypeId Param;
        NodeTypeId Sample;
        NodeTypeId Output;

        Fixture()
        {
            const Veng::TypeId f32Id = TypeIdOf<Veng::f32>();

            Source = Catalog.Register(NodeType{
                .Name = "Source",
                .Outputs = {PinDesc{"Out", ValueOf(f32Id)}},
            });

            Param = Catalog.Register(NodeType{
                .Name = "Param",
                .Outputs = {PinDesc{"Out", ValueOf(f32Id)}},
                .Properties = {ValueField()},
                .PropertySize = sizeof(ValueProps),
            });

            Sample = Catalog.Register(NodeType{
                .Name = "Sample",
                .Outputs = {PinDesc{"Out", ValueOf(f32Id)}},
                .Properties = {TextureField()},
                .PropertySize = sizeof(TextureProps),
            });

            Output = Catalog.Register(NodeType{
                .Name = "Output",
                .Inputs = {PinDesc{"In", ValueOf(f32Id)}},
            });
        }

        NodeGraph MakeGraph() const
        {
            return NodeGraph([](const PinType& from, const PinType& to)
                             { return from.Type == to.Type; },
                             [this](NodeTypeId id) { return Catalog.ShapeOf(id); },
                             [this](NodeTypeId id)
                             {
                                 const NodeType* type = Catalog.Find(id);
                                 return type ? type->PropertySize : Veng::usize{0};
                             });
        }
    };

    Veng::vec4 ReadValueProperty(const NodeGraph& graph, NodeId node,
                                 const Veng::FieldDescriptor& field)
    {
        const std::span<const std::byte> bytes = graph.PropertyBytes(node);
        Veng::vec4 out{};
        std::memcpy(&out, bytes.data() + field.Offset, sizeof(out));
        return out;
    }

    Veng::u64 ReadAssetIdProperty(const NodeGraph& graph, NodeId node,
                                  const Veng::FieldDescriptor& field)
    {
        const std::span<const std::byte> bytes = graph.PropertyBytes(node);
        Veng::u64 id = 0;
        std::memcpy(&id, bytes.data() + field.Offset, sizeof(id));
        return id;
    }
}

TEST_CASE("NodeCatalog: Find round-trips by id and by name")
{
    Fixture fx;

    const NodeType* byId = fx.Catalog.Find(fx.Param);
    REQUIRE(byId != nullptr);
    CHECK(byId->Name == "Param");
    CHECK(byId->Id == fx.Param);

    const NodeType* byName = fx.Catalog.Find("Param");
    REQUIRE(byName != nullptr);
    CHECK(byName->Id == fx.Param);

    CHECK(fx.Catalog.Find("Nonexistent") == nullptr);
    CHECK(fx.Catalog.Find(NodeTypeId{0}) == nullptr);
    CHECK(fx.Catalog.Find(NodeTypeId{9999}) == nullptr);
    CHECK(fx.Catalog.Types().size() == 4);
}

TEST_CASE("NodeGraphSerialize: nodes, links, positions, and a vec4 property round-trip")
{
    Fixture fx;
    NodeGraph graph = fx.MakeGraph();

    const NodeId param = graph.AddNode(fx.Param);
    const NodeId output = graph.AddNode(fx.Output);
    graph.MoveNode(param, Veng::vec2{12.0f, -4.0f});
    graph.MoveNode(output, Veng::vec2{200.0f, 50.0f});

    const Veng::FieldDescriptor value = ValueField();
    const Veng::vec4 expected{1.0f, 2.0f, 3.0f, 4.0f};
    graph.SetProperty(param, value,
                      std::span<const std::byte>(reinterpret_cast<const std::byte*>(&expected),
                                                 sizeof(expected)));

    REQUIRE(graph.Connect(PinRef{param, 0}, PinRef{output, 0}).has_value());

    const Veng::string doc = WriteNodeGraph(graph, fx.Catalog);

    NodeGraph loaded = fx.MakeGraph();
    const NodeGraphReadOutcome outcome = ReadNodeGraph(doc, loaded, fx.Catalog);
    CHECK(outcome == NodeGraphReadOutcome::Loaded);

    REQUIRE(loaded.Nodes().size() == 2);
    CHECK(loaded.Links().size() == 1);

    // Find the Param node back by its type and read its property.
    NodeId loadedParam{};
    NodeId loadedOutput{};
    for (NodeId n : loaded.Nodes())
    {
        if (loaded.GetTypeOf(n) == fx.Param)
            loadedParam = n;
        if (loaded.GetTypeOf(n) == fx.Output)
            loadedOutput = n;
    }
    REQUIRE(loaded.IsValid(loadedParam));
    REQUIRE(loaded.IsValid(loadedOutput));

    CHECK(loaded.PositionOf(loadedParam) == Veng::vec2{12.0f, -4.0f});
    CHECK(loaded.PositionOf(loadedOutput) == Veng::vec2{200.0f, 50.0f});
    CHECK(ReadValueProperty(loaded, loadedParam, value) == expected);

    // The link reconnects param.Out -> output.In.
    const Link& link = loaded.Links()[0];
    CHECK(link.From.Node == loadedParam);
    CHECK(link.To.Node == loadedOutput);
}

TEST_CASE(
    "NodeGraphSerialize: an AssetHandle property persists and rehydrates; invalid is no asset")
{
    Fixture fx;
    const Veng::FieldDescriptor texture = TextureField();

    SUBCASE("a valid id round-trips")
    {
        NodeGraph graph = fx.MakeGraph();
        const NodeId sample = graph.AddNode(fx.Sample);

        // An AssetHandle stores its AssetId at offset 0; write a raw id into the
        // handle-sized leading u64.
        const Veng::u64 id = 0x4DD9F2A1C03B5E76ULL;
        std::byte handleBytes[sizeof(Veng::AssetHandle<Veng::Texture>)] = {};
        std::memcpy(handleBytes, &id, sizeof(id));
        graph.SetProperty(sample, texture,
                          std::span<const std::byte>(handleBytes, sizeof(Veng::u64)));

        const Veng::string doc = WriteNodeGraph(graph, fx.Catalog);

        NodeGraph loaded = fx.MakeGraph();
        CHECK(ReadNodeGraph(doc, loaded, fx.Catalog) == NodeGraphReadOutcome::Loaded);
        REQUIRE(loaded.Nodes().size() == 1);
        const NodeId loadedSample = loaded.Nodes()[0];
        CHECK(ReadAssetIdProperty(loaded, loadedSample, texture) == id);
    }

    SUBCASE("an invalid id serializes as no asset")
    {
        NodeGraph graph = fx.MakeGraph();
        const NodeId sample = graph.AddNode(fx.Sample);
        // No SetProperty: the id stays zero (invalid).

        const Veng::string docStr = WriteNodeGraph(graph, fx.Catalog);

        // The property serializes to null.
        const nlohmann::json doc = nlohmann::json::parse(docStr);
        REQUIRE(doc["nodes"][0]["properties"]["Texture"].is_null());

        NodeGraph loaded = fx.MakeGraph();
        CHECK(ReadNodeGraph(docStr, loaded, fx.Catalog) == NodeGraphReadOutcome::Loaded);
        const NodeId loadedSample = loaded.Nodes()[0];
        CHECK(ReadAssetIdProperty(loaded, loadedSample, texture) == 0);
    }
}

TEST_CASE("NodeGraphSerialize: an unknown node type is dropped, the rest loads")
{
    Fixture fx;
    NodeGraph graph = fx.MakeGraph();
    const NodeId source = graph.AddNode(fx.Source);
    const NodeId output = graph.AddNode(fx.Output);
    REQUIRE(graph.Connect(PinRef{source, 0}, PinRef{output, 0}).has_value());

    nlohmann::json doc = nlohmann::json::parse(WriteNodeGraph(graph, fx.Catalog));

    // Inject a third node of a type absent from the catalog, wired to Output.
    nlohmann::json ghost = nlohmann::json::object();
    ghost["type"] = "GhostType";
    ghost["position"] = nlohmann::json::array({0.0f, 0.0f});
    doc["nodes"].push_back(ghost);

    nlohmann::json ghostLink = nlohmann::json::object();
    ghostLink["from_node"] = 2; // the ghost
    ghostLink["from_pin"] = "Out";
    ghostLink["to_node"] = 1; // output
    ghostLink["to_pin"] = "In";
    doc["links"].push_back(ghostLink);

    NodeGraph loaded = fx.MakeGraph();
    const NodeGraphReadOutcome outcome = ReadNodeGraph(doc.dump(), loaded, fx.Catalog);
    CHECK(outcome == NodeGraphReadOutcome::Loaded);

    // The two real nodes loaded; the ghost was dropped, and the original link
    // between Source and Output survives.
    CHECK(loaded.Nodes().size() == 2);
    CHECK(loaded.Links().size() == 1);
}

TEST_CASE("NodeGraphSerialize: a version newer than the format is read-only, no partial graph")
{
    Fixture fx;
    NodeGraph graph = fx.MakeGraph();
    graph.AddNode(fx.Source);
    graph.AddNode(fx.Output);

    nlohmann::json doc = nlohmann::json::parse(WriteNodeGraph(graph, fx.Catalog));
    doc["version"] = NodeGraphFormatVersion + 1;

    NodeGraph loaded = fx.MakeGraph();
    const NodeGraphReadOutcome outcome = ReadNodeGraph(doc.dump(), loaded, fx.Catalog);
    CHECK(outcome == NodeGraphReadOutcome::VersionTooNew);

    // Nothing was written into the destination.
    CHECK(loaded.Nodes().empty());
    CHECK(loaded.Links().empty());
}
