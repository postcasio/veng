#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/FieldDescriptor.h>

#include <cstddef>
#include <span>

namespace VengEditor
{
    /// @brief Editor-local id naming a node type in the catalog.
    ///
    /// Not a runtime TypeId; node types are not registered in the engine's
    /// TypeRegistry. The topology core stores it on a node and passes it to
    /// PinShapeFn; the catalog assigns and resolves the ids.
    struct NodeTypeId
    {
        /// @brief Catalog-assigned id value; 0 is the unset sentinel.
        Veng::u32 Value = 0;

        /// @brief Member-wise equality on the id value.
        bool operator==(const NodeTypeId&) const = default;
    };

    /// @brief Generational handle into the graph's node table.
    ///
    /// The generation bumps every time a slot is recycled, so a NodeId whose slot
    /// was reused never silently aliases a new node.
    struct NodeId
    {
        /// @brief Slot index in the node table.
        Veng::u32 Index = 0;
        /// @brief Generation counter; must match the slot's current generation.
        Veng::u32 Generation = 0;

        /// @brief Member-wise equality on index and generation.
        bool operator==(const NodeId&) const = default;
    };

    /// @brief A pin on a node: the node plus its slot index.
    ///
    /// Whether a slot is an input or an output is determined by the node type's
    /// pin shape, not encoded in the ref itself.
    struct PinRef
    {
        /// @brief The node that owns this pin.
        NodeId Node;
        /// @brief Zero-based index into the node's input or output pin list.
        Veng::u16 Pin = 0;

        /// @brief Member-wise equality on the node and pin index.
        bool operator==(const PinRef&) const = default;
    };

    /// @brief A pin's data type.
    ///
    /// A Value pin carries a builtin leaf TypeId (vec4, f32, …); a Wildcard pin
    /// accepts any type. Assets are node properties, not pins.
    struct PinType
    {
        /// @brief Discriminator for Value vs. Wildcard pins.
        enum class Kind : Veng::u8 { Value, Wildcard };

        /// @brief Whether this pin is typed or wildcard.
        Kind Kind = Kind::Value;
        /// @brief Builtin leaf TypeId; meaningful only when Kind == Value.
        Veng::TypeId Type = Veng::InvalidTypeId;
    };

    /// @brief A directed edge in the graph. From names an output pin; To names an input pin.
    struct Link
    {
        /// @brief Source (output) end of the edge.
        PinRef From;
        /// @brief Destination (input) end of the edge.
        PinRef To;

        /// @brief Member-wise equality on both endpoints.
        bool operator==(const Link&) const = default;
    };

    /// @brief Input and output pin types of a node type.
    ///
    /// Returned by PinShapeFn for a given NodeTypeId; the topology core reads it
    /// to validate connections.
    struct NodePinShape
    {
        /// @brief Types of the node's input pins, in order.
        Veng::vector<PinType> Inputs;
        /// @brief Types of the node's output pins, in order.
        Veng::vector<PinType> Outputs;
    };

    /// @brief Domain-supplied connection predicate.
    ///
    /// Acyclicity, direction, and arity are enforced generically by the graph.
    /// Type compatibility — including coercions — is the domain's responsibility.
    /// Wildcard pins are resolved by the graph and never reach this hook.
    using CanConnectFn = Veng::function<bool(const PinType& from, const PinType& to)>;

    /// @brief Maps a NodeTypeId to its input/output pin shape.
    ///
    /// Supplied at NodeGraph construction so the topology core stays self-contained.
    using PinShapeFn = Veng::function<NodePinShape(NodeTypeId type)>;

    /// @brief Maps a NodeTypeId to the byte size of its property struct.
    ///
    /// The graph allocates each node's opaque property buffer to this size on AddNode.
    using PropertySizeFn = Veng::function<Veng::usize(NodeTypeId type)>;

    /// @brief Pure, generic node-graph topology: data, mutation vocabulary, and
    /// validation (direction, arity, acyclicity).
    ///
    /// Knows nothing of ImGui, Vulkan, or any domain. Domain knowledge enters only
    /// through the three construction hooks. The graph is a DAG by construction.
    class NodeGraph
    {
    public:
        /// @brief Constructs a NodeGraph with the given domain hooks.
        /// @param canConnect  Predicate for type-compatibility between pins.
        /// @param pinShape    Returns the input/output pin shape for a node type.
        /// @param propertySize Returns the property buffer size for a node type.
        NodeGraph(CanConnectFn canConnect, PinShapeFn pinShape,
                  PropertySizeFn propertySize);

        /// @brief Adds a node of the given type and returns its id.
        NodeId AddNode(NodeTypeId type);
        /// @brief Removes a node and all links incident to it.
        void RemoveNode(NodeId node);
        /// @brief Connects an output pin to an input pin.
        ///
        /// Validates direction, arity, type compatibility, and acyclicity.
        /// @return An error string on failure; success on ok.
        Veng::VoidResult Connect(PinRef from, PinRef to);
        /// @brief Removes the given link from the graph. No-op if not present.
        void Disconnect(const Link& link);
        /// @brief Sets the canvas position of a node. No-op on a stale node.
        void MoveNode(NodeId node, Veng::vec2 canvasPos);

        /// @brief Writes bytes into a node's property buffer at the field's offset.
        ///
        /// No-op on a stale node. The byte count must match the field type's size
        /// and lie within the node's buffer (both asserted).
        /// @param node  Target node.
        /// @param field Descriptor giving the offset and type of the property.
        /// @param bytes Source bytes; size must equal the field type's size.
        void SetProperty(NodeId node, const Veng::FieldDescriptor& field,
                         std::span<const std::byte> bytes);

        /// @brief Returns true when node refers to a live slot with a matching generation.
        [[nodiscard]] bool IsValid(NodeId node) const;
        /// @brief Returns the catalog type of a live node. Asserts on a stale node.
        [[nodiscard]] NodeTypeId GetTypeOf(NodeId node) const;
        /// @brief Returns the ids of all live nodes, in creation order.
        [[nodiscard]] std::span<const NodeId> Nodes() const;
        /// @brief Returns all directed edges in the graph.
        [[nodiscard]] std::span<const Link> Links() const;
        /// @brief Returns the canvas position of a live node. Asserts on a stale node.
        [[nodiscard]] Veng::vec2 PositionOf(NodeId node) const;

        /// @brief Returns the node's opaque property buffer (read-only).
        ///
        /// The serializer and inspector walk fields out of this span using
        /// FieldDescriptor offsets. Empty when the type has no properties.
        [[nodiscard]] std::span<const std::byte> PropertyBytes(NodeId node) const;

        /// @brief Returns a stable topological ordering over the DAG.
        ///
        /// Ties resolve by node-creation order, making the result deterministic.
        [[nodiscard]] Veng::vector<NodeId> TopoOrder() const;

    private:
        struct Node
        {
            NodeTypeId Type;
            Veng::u32 Generation = 0;
            bool Alive = false;
            Veng::vec2 Position{0.0f, 0.0f};
            /// @brief Opaque property buffer, sized to the type's PropertySize and
            /// zero-initialised on AddNode; the reflection layer addresses fields by FieldDescriptor.Offset.
            Veng::vector<std::byte> Properties;
        };

        [[nodiscard]] const Node* Lookup(NodeId node) const;
        [[nodiscard]] NodePinShape ShapeOf(NodeId node) const;

        /// @brief Returns true when target is reachable from origin over existing links.
        ///
        /// Used to detect whether a new edge origin→target would close a cycle.
        [[nodiscard]] bool Reaches(NodeId origin, NodeId target) const;

        CanConnectFn m_CanConnect;
        PinShapeFn m_PinShape;
        PropertySizeFn m_PropertySize;

        Veng::vector<Node> m_Nodes;
        /// @brief Recycled slot indices available for reuse.
        Veng::vector<Veng::u32> m_FreeList;
        /// @brief Compact list of live node ids, in creation order.
        Veng::vector<NodeId> m_Live;
        Veng::vector<Link> m_Links;
    };
}
