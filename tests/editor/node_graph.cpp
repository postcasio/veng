// NodeGraph topology-core unit cases: the generic, pure graph — add/remove with
// generational invalidation, Connect validation (direction/arity/type/cycle),
// incident-link cleanup on RemoveNode, a stable diamond TopoOrder, and wildcard
// pins. Device-free; no Context, no Vulkan symbol touched.

#include <doctest/doctest.h>

#include <VengEditor/NodeGraph/NodeGraph.h>

#include <Veng/Reflection/TypeId.h>

#include <algorithm>

using namespace VengEditor;
using Veng::TypeIdOf;

namespace
{
    // Node-type catalog for the tests. Each id names a fixed pin shape:
    //   Source   — no inputs, one f32 output
    //   Passthru — one f32 input, one f32 output
    //   Sink     — one f32 input, no outputs
    //   Mismatch — no inputs, one vec4 output (incompatible with an f32 input)
    //   WildOut  — no inputs, one wildcard output
    //   WildIn   — one wildcard input, one f32 output
    enum Type : Veng::u32
    {
        Source = 1,
        Passthru,
        Sink,
        Mismatch,
        WildOut,
        WildIn
    };

    PinType ValueOf(Veng::TypeId id)
    {
        return PinType{.Kind = PinType::Kind::Value, .Type = id};
    }
    PinType Wildcard()
    {
        return PinType{.Kind = PinType::Kind::Wildcard, .Type = Veng::InvalidTypeId};
    }

    NodePinShape ShapeFor(NodeTypeId type)
    {
        const Veng::TypeId f32Id = TypeIdOf<Veng::f32>();
        const Veng::TypeId vec4Id = TypeIdOf<Veng::vec4>();
        switch (type.Value)
        {
        case Source:
            return NodePinShape{.Inputs = {}, .Outputs = {ValueOf(f32Id)}};
        case Passthru:
            return NodePinShape{.Inputs = {ValueOf(f32Id)}, .Outputs = {ValueOf(f32Id)}};
        case Sink:
            return NodePinShape{.Inputs = {ValueOf(f32Id)}, .Outputs = {}};
        case Mismatch:
            return NodePinShape{.Inputs = {}, .Outputs = {ValueOf(vec4Id)}};
        case WildOut:
            return NodePinShape{.Inputs = {}, .Outputs = {Wildcard()}};
        case WildIn:
            return NodePinShape{.Inputs = {Wildcard()}, .Outputs = {ValueOf(f32Id)}};
        default:
            return NodePinShape{};
        }
    }

    // Strict type-equality predicate: the domain accepts only an exact TypeId
    // match. Wildcard never reaches here (the graph short-circuits it).
    CanConnectFn ExactMatch()
    {
        return [](const PinType& from, const PinType& to) { return from.Type == to.Type; };
    }

    // These topology-core cases carry no node properties; the size hook is a
    // constant zero.
    Veng::usize NoProperties(NodeTypeId)
    {
        return 0;
    }

    NodeGraph MakeGraph()
    {
        return NodeGraph(ExactMatch(), ShapeFor, NoProperties);
    }

    bool HasLink(const NodeGraph& graph, NodeId from, NodeId to)
    {
        for (const Link& link : graph.Links())
        {
            if (link.From.Node == from && link.To.Node == to)
            {
                return true;
            }
        }
        return false;
    }

    Veng::usize OrderIndex(const Veng::vector<NodeId>& order, NodeId node)
    {
        const auto it = std::ranges::find(order, node);
        return static_cast<Veng::usize>(it - order.begin());
    }
}

TEST_CASE("NodeGraph: add then remove invalidates the NodeId generationally")
{
    NodeGraph graph = MakeGraph();

    const NodeId a = graph.AddNode(NodeTypeId{Source});
    CHECK(graph.IsValid(a));
    CHECK(graph.Nodes().size() == 1);

    graph.RemoveNode(a);
    CHECK_FALSE(graph.IsValid(a));
    CHECK(graph.Nodes().size() == 0);

    // The next add recycles the slot but bumps the generation, so the stale id
    // never aliases the new node.
    const NodeId b = graph.AddNode(NodeTypeId{Source});
    CHECK(graph.IsValid(b));
    CHECK_FALSE(graph.IsValid(a));
    CHECK(a.Index == b.Index);
    CHECK(a.Generation != b.Generation);

    // A second RemoveNode of the now-stale id is a harmless no-op.
    graph.RemoveNode(a);
    CHECK(graph.IsValid(b));
}

TEST_CASE("NodeGraph: Connect accepts a valid compatible output->input")
{
    NodeGraph graph = MakeGraph();
    const NodeId src = graph.AddNode(NodeTypeId{Source});
    const NodeId sink = graph.AddNode(NodeTypeId{Sink});

    const Veng::VoidResult result =
        graph.Connect(PinRef{.Node = src, .Pin = 0}, PinRef{.Node = sink, .Pin = 0});
    CHECK(result.has_value());
    CHECK(graph.Links().size() == 1);
    CHECK(HasLink(graph, src, sink));
}

TEST_CASE("NodeGraph: Connect rejects wrong direction")
{
    NodeGraph graph = MakeGraph();
    const NodeId src = graph.AddNode(NodeTypeId{Source});
    const NodeId sink = graph.AddNode(NodeTypeId{Sink});

    // src has no input pin (pin 0 is its output); sink has no output pin. Naming
    // sink's pin 0 as From (an output) is out of range -> rejected.
    const Veng::VoidResult result =
        graph.Connect(PinRef{.Node = sink, .Pin = 0}, PinRef{.Node = src, .Pin = 0});
    CHECK_FALSE(result.has_value());
    CHECK(graph.Links().empty());
}

TEST_CASE("NodeGraph: Connect rejects a double-booked input pin")
{
    NodeGraph graph = MakeGraph();
    const NodeId a = graph.AddNode(NodeTypeId{Source});
    const NodeId b = graph.AddNode(NodeTypeId{Source});
    const NodeId sink = graph.AddNode(NodeTypeId{Sink});

    CHECK(graph.Connect(PinRef{a, 0}, PinRef{sink, 0}).has_value());

    // The single input pin already holds a link; a second is rejected.
    const Veng::VoidResult second =
        graph.Connect(PinRef{.Node = b, .Pin = 0}, PinRef{.Node = sink, .Pin = 0});
    CHECK_FALSE(second.has_value());
    CHECK(graph.Links().size() == 1);

    // An output pin fans out freely.
    const NodeId sink2 = graph.AddNode(NodeTypeId{Sink});
    CHECK(graph.Connect(PinRef{a, 0}, PinRef{sink2, 0}).has_value());
    CHECK(graph.Links().size() == 2);
}

TEST_CASE("NodeGraph: Connect rejects incompatible types via the domain predicate")
{
    NodeGraph graph = MakeGraph();
    const NodeId mismatch = graph.AddNode(NodeTypeId{Mismatch}); // vec4 output
    const NodeId sink = graph.AddNode(NodeTypeId{Sink});         // f32 input

    const Veng::VoidResult result =
        graph.Connect(PinRef{.Node = mismatch, .Pin = 0}, PinRef{.Node = sink, .Pin = 0});
    CHECK_FALSE(result.has_value());
    CHECK(graph.Links().empty());
}

TEST_CASE("NodeGraph: Connect rejects an edge that would introduce a cycle")
{
    NodeGraph graph = MakeGraph();
    const NodeId a = graph.AddNode(NodeTypeId{Passthru});
    const NodeId b = graph.AddNode(NodeTypeId{Passthru});

    CHECK(graph.Connect(PinRef{a, 0}, PinRef{b, 0}).has_value());

    // b -> a would close the a -> b -> a loop.
    const Veng::VoidResult back =
        graph.Connect(PinRef{.Node = b, .Pin = 0}, PinRef{.Node = a, .Pin = 0});
    CHECK_FALSE(back.has_value());
    CHECK(graph.Links().size() == 1);

    // A self-loop is also a cycle.
    const Veng::VoidResult self =
        graph.Connect(PinRef{.Node = a, .Pin = 0}, PinRef{.Node = a, .Pin = 0});
    CHECK_FALSE(self.has_value());
}

TEST_CASE("NodeGraph: RemoveNode drops incident links from both endpoints")
{
    NodeGraph graph = MakeGraph();
    const NodeId src = graph.AddNode(NodeTypeId{Source});
    const NodeId mid = graph.AddNode(NodeTypeId{Passthru});
    const NodeId sink = graph.AddNode(NodeTypeId{Sink});

    CHECK(graph.Connect(PinRef{src, 0}, PinRef{mid, 0}).has_value());
    CHECK(graph.Connect(PinRef{mid, 0}, PinRef{sink, 0}).has_value());
    CHECK(graph.Links().size() == 2);

    graph.RemoveNode(mid);
    CHECK(graph.Links().empty()); // both incident links gone
    CHECK(graph.IsValid(src));
    CHECK(graph.IsValid(sink));
}

TEST_CASE("NodeGraph: Disconnect removes exactly the named link")
{
    NodeGraph graph = MakeGraph();
    const NodeId src = graph.AddNode(NodeTypeId{Source});
    const NodeId sink = graph.AddNode(NodeTypeId{Sink});

    CHECK(graph.Connect(PinRef{src, 0}, PinRef{sink, 0}).has_value());
    REQUIRE(graph.Links().size() == 1);

    graph.Disconnect(graph.Links()[0]);
    CHECK(graph.Links().empty());
}

TEST_CASE("NodeGraph: MoveNode and PositionOf round-trip")
{
    NodeGraph graph = MakeGraph();
    const NodeId n = graph.AddNode(NodeTypeId{Source});
    CHECK(graph.PositionOf(n) == Veng::vec2{0.0f, 0.0f});

    graph.MoveNode(n, Veng::vec2{12.0f, -4.0f});
    CHECK(graph.PositionOf(n) == Veng::vec2{12.0f, -4.0f});
}

TEST_CASE("NodeGraph: TopoOrder is a valid, stable ordering on a diamond")
{
    // A four-node diamond: top -> left, top -> right, left -> bottom,
    // right -> bottom. top has one output, the middles one input + one output,
    // bottom two inputs.
    NodeGraph graph(
        ExactMatch(),
        [](NodeTypeId type) -> NodePinShape
        {
            const Veng::TypeId f32Id = TypeIdOf<Veng::f32>();
            const PinType v{.Kind = PinType::Kind::Value, .Type = f32Id};
            switch (type.Value)
            {
            case 100:
                return NodePinShape{.Inputs = {}, .Outputs = {v}}; // top: 1 out
            case 101:
                return NodePinShape{.Inputs = {v}, .Outputs = {v}}; // middle: 1 in, 1 out
            case 102:
                return NodePinShape{.Inputs = {v, v}, .Outputs = {}}; // bottom: 2 in, 0 out
            default:
                return NodePinShape{};
            }
        },
        NoProperties);

    const NodeId top = graph.AddNode(NodeTypeId{100});
    const NodeId left = graph.AddNode(NodeTypeId{101});
    const NodeId right = graph.AddNode(NodeTypeId{101});
    const NodeId bottom = graph.AddNode(NodeTypeId{102});

    CHECK(graph.Connect(PinRef{top, 0}, PinRef{left, 0}).has_value());
    CHECK(graph.Connect(PinRef{top, 0}, PinRef{right, 0}).has_value());
    CHECK(graph.Connect(PinRef{left, 0}, PinRef{bottom, 0}).has_value());
    CHECK(graph.Connect(PinRef{right, 0}, PinRef{bottom, 1}).has_value());

    const Veng::vector<NodeId> order = graph.TopoOrder();
    REQUIRE(order.size() == 4);

    // Every edge points forward in the ordering.
    CHECK(OrderIndex(order, top) < OrderIndex(order, left));
    CHECK(OrderIndex(order, top) < OrderIndex(order, right));
    CHECK(OrderIndex(order, left) < OrderIndex(order, bottom));
    CHECK(OrderIndex(order, right) < OrderIndex(order, bottom));

    // Stable: ties resolve by creation order, so left precedes right.
    CHECK(OrderIndex(order, left) < OrderIndex(order, right));

    // Re-querying yields the identical ordering.
    CHECK(graph.TopoOrder() == order);
}

TEST_CASE("NodeGraph: wildcard pins connect to any value pin")
{
    NodeGraph graph = MakeGraph();

    // A wildcard output feeds a concrete f32 input.
    const NodeId wildOut = graph.AddNode(NodeTypeId{WildOut});
    const NodeId sink = graph.AddNode(NodeTypeId{Sink});
    CHECK(graph.Connect(PinRef{wildOut, 0}, PinRef{sink, 0}).has_value());

    // A concrete f32 output feeds a wildcard input — even though the strict
    // domain predicate would reject a vec4, the wildcard short-circuits it.
    const NodeId mismatch = graph.AddNode(NodeTypeId{Mismatch}); // vec4 output
    const NodeId wildIn = graph.AddNode(NodeTypeId{WildIn});     // wildcard input
    CHECK(graph.Connect(PinRef{mismatch, 0}, PinRef{wildIn, 0}).has_value());
}
