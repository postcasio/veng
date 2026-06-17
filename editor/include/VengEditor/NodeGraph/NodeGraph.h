#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/FieldDescriptor.h>

#include <cstddef>
#include <span>

namespace VengEditor
{
    // A catalog id naming a node type inside the editor. Editor-local, not a
    // runtime TypeId: node types are not registered into the engine's
    // TypeRegistry. The topology core stores it on a node and never interprets it
    // beyond handing it to the construction-time PinShapeFn to ask for a node's
    // pin shape; the catalog assigns and resolves the ids.
    struct NodeTypeId
    {
        Veng::u32 Value = 0;

        bool operator==(const NodeTypeId&) const = default;
    };

    // A generational handle into the graph's node table — the Entity pattern. The
    // generation bumps every time a slot is recycled, so a NodeId whose slot was
    // reused never silently aliases a new node.
    struct NodeId
    {
        Veng::u32 Index = 0;
        Veng::u32 Generation = 0;

        bool operator==(const NodeId&) const = default;
    };

    // A pin on a node: the node plus its slot index. Whether a slot is an input
    // or an output is the node type's pin shape, not encoded in the ref.
    struct PinRef
    {
        NodeId Node;
        Veng::u16 Pin = 0;

        bool operator==(const PinRef&) const = default;
    };

    // A pin's data type. A Value pin carries a builtin leaf TypeId (vec4, f32,
    // ...); a Wildcard pin accepts any type (a math node's "T"). Assets are node
    // properties, not pins.
    struct PinType
    {
        enum class Kind : Veng::u8 { Value, Wildcard };

        Kind Kind = Kind::Value;
        Veng::TypeId Type = Veng::InvalidTypeId; // meaningful when Kind == Value
    };

    // A directed edge in the graph. From names an output pin; To names an input
    // pin.
    struct Link
    {
        PinRef From;
        PinRef To;

        bool operator==(const Link&) const = default;
    };

    // The input and output pin types of a node type. The PinShapeFn returns this
    // for a given NodeTypeId; the topology core reads it to validate connections.
    struct NodePinShape
    {
        Veng::vector<PinType> Inputs;
        Veng::vector<PinType> Outputs;
    };

    // Domain-supplied connection predicate. Acyclicity, direction, and arity are
    // generic (enforced by the graph); type compatibility — including coercions —
    // is the domain's. Wildcard is handled by the graph and never reaches this
    // hook.
    using CanConnectFn = Veng::function<bool(const PinType& from, const PinType& to)>;

    // Node type -> its input/output pin shape. The topology core takes this at
    // construction so it stays self-contained; the catalog supplies the real one.
    using PinShapeFn = Veng::function<NodePinShape(NodeTypeId type)>;

    // Node type -> the byte size of its property struct. The graph allocates a
    // node's opaque property buffer to this size on AddNode; the catalog supplies
    // it from the type's PropertySize.
    using PropertySizeFn = Veng::function<Veng::usize(NodeTypeId type)>;

    // A pure, generic node-graph topology: data, the mutation vocabulary, and
    // generic validation (direction/arity/acyclicity). It knows nothing of ImGui,
    // Vulkan, or "material"; the only domain knowledge enters through the two
    // construction hooks. The graph is a DAG by construction.
    class NodeGraph
    {
    public:
        NodeGraph(CanConnectFn canConnect, PinShapeFn pinShape,
                  PropertySizeFn propertySize);

        // --- the mutation vocabulary (the only way the model changes) ---

        NodeId AddNode(NodeTypeId type);
        void RemoveNode(NodeId node); // also drops incident links
        Veng::VoidResult Connect(PinRef from, PinRef to);
        void Disconnect(const Link& link);
        void MoveNode(NodeId node, Veng::vec2 canvasPos);

        // Writes one property's bytes into the node's property buffer at the
        // descriptor's offset. The writable path the future undo stack wraps; a
        // no-op on a stale node. The byte count must match the field type's size
        // (asserted) and lie within the node's buffer.
        void SetProperty(NodeId node, const Veng::FieldDescriptor& field,
                         std::span<const std::byte> bytes);

        // --- queries ---

        [[nodiscard]] bool IsValid(NodeId node) const;
        [[nodiscard]] NodeTypeId GetTypeOf(NodeId node) const;
        [[nodiscard]] std::span<const NodeId> Nodes() const;
        [[nodiscard]] std::span<const Link> Links() const;
        [[nodiscard]] Veng::vec2 PositionOf(NodeId node) const;

        // The node's opaque property buffer, read-only — the span the serializer
        // and the inspector's draw walk read fields out of. Empty when the type
        // has no properties.
        [[nodiscard]] std::span<const std::byte> PropertyBytes(NodeId node) const;

        // A topological ordering over the DAG, for a compiler to walk. Stable:
        // ties resolve by node-creation order.
        [[nodiscard]] Veng::vector<NodeId> TopoOrder() const;

    private:
        struct Node
        {
            NodeTypeId Type;
            Veng::u32 Generation = 0;
            bool Alive = false;
            Veng::vec2 Position{0.0f, 0.0f};
            // Sized to the type's PropertySize, zero-initialised on AddNode; the
            // reflection layer addresses fields into it by FieldDescriptor.Offset.
            Veng::vector<std::byte> Properties;
        };

        [[nodiscard]] const Node* Lookup(NodeId node) const;
        [[nodiscard]] NodePinShape ShapeOf(NodeId node) const;

        // True when target is reachable from origin over existing links — the new
        // edge origin->target would then close a cycle.
        [[nodiscard]] bool Reaches(NodeId origin, NodeId target) const;

        CanConnectFn m_CanConnect;
        PinShapeFn m_PinShape;
        PropertySizeFn m_PropertySize;

        Veng::vector<Node> m_Nodes;
        Veng::vector<Veng::u32> m_FreeList; // recycled slot indices
        Veng::vector<NodeId> m_Live;        // compact list of live node ids
        Veng::vector<Link> m_Links;
    };
}
