# Plan 01 — NodeGraph topology core (Layer 1)

**Goal:** the generic, pure node-graph topology — the foundation everything else builds on,
landed as a **named `libveng_editor` surface** under `VengEditor/NodeGraph/`. No ImGui, no
Vulkan, and no mention of "material": this layer is data + operations + validation, and is
**unit-tested device-free** (the `DecideBarrier` / transient live-range pattern).

## Surface

`editor/include/VengEditor/NodeGraph/NodeGraph.h`:

```cpp
namespace VengEditor
{
    struct NodeId   { Veng::u32 Index = 0; Veng::u32 Generation = 0; };   // mirrors Entity
    struct PinRef   { NodeId Node; Veng::u16 Pin = 0; };                  // a node's slot

    // A pin's data type. Value pins carry a builtin leaf TypeId (vec4, f32, ...);
    // Wildcard accepts any (a math node's "T"). Assets are NOT pins (decision 6).
    struct PinType
    {
        enum class Kind : Veng::u8 { Value, Wildcard };
        Kind Kind = Kind::Value;
        Veng::TypeId Type = Veng::InvalidTypeId;   // meaningful when Kind == Value
    };

    struct Link { PinRef From; PinRef To; };        // From = an output, To = an input

    // Domain-supplied connection predicate. Acyclicity/direction/arity are generic
    // (enforced below); type compatibility — including coercions — is the domain's.
    using CanConnectFn = Veng::function<bool(const PinType& from, const PinType& to)>;

    class NodeGraph
    {
    public:
        explicit NodeGraph(CanConnectFn canConnect);

        // --- the mutation vocabulary (the only way the model changes) ---
        NodeId AddNode(NodeTypeId type);            // NodeType lookup is Layer 2's
        void   RemoveNode(NodeId);                  // also drops incident links
        Veng::VoidResult Connect(PinRef from, PinRef to);   // recoverable: see below
        void   Disconnect(const Link&);
        void   MoveNode(NodeId, Veng::vec2 canvasPos);
        // SetProperty lives with the instance store in Layer 2.

        // --- queries ---
        [[nodiscard]] bool IsValid(NodeId) const;
        [[nodiscard]] std::span<const NodeId> Nodes() const;
        [[nodiscard]] std::span<const Link>   Links() const;
        [[nodiscard]] Veng::vec2 PositionOf(NodeId) const;
        // Topological order over the DAG, for the compiler to walk (Layer 3).
        [[nodiscard]] Veng::vector<NodeId> TopoOrder() const;
    };
}
```

`NodeTypeId` is a forward-declared editor-local catalog id (defined in Layer 2); Layer 1
stores it on a node and never interprets it beyond "which type descriptor to ask Layer 2 for
the pin shape." To keep Layer 1 self-contained for its own tests, it takes a small
`PinShapeFn` (node type → input/output `PinType`s) at construction alongside `CanConnectFn`,
rather than reaching into the catalog. Layer 2 supplies the real one.

## Validation rules (generic, owned here)

`Connect` returns `VoidResult` and **fails recoverably** (it is user input, not API misuse)
on:

- **Direction** — `From` must be an output pin, `To` an input pin.
- **Arity** — an input pin holds at most one link; connecting a second replaces or is
  rejected (reject; the panel disconnects first). Output pins fan out freely.
- **Type** — `CanConnect(fromType, toType)` is false. Wildcard is compatible with anything.
- **Acyclicity** — the new link would introduce a cycle (reachability check from `To` to
  `From` over existing links). The graph is a DAG by construction.

A `RemoveNode` of a non-existent (stale-generation) node is a no-op; an `IsValid` check
gates all queries. Node storage is a vector + generational free-list (the `Entity` pattern),
so a `NodeId` whose slot was recycled never silently aliases a new node.

## Why pure

The whole layer compiles with only `<Veng/Veng.h>` + `<Veng/Reflection/TypeId.h>` — no
ImGui, no Vulkan, no imnodes. That is what makes it unit-testable with no device and keeps it
reusable by any future editor. Mutation is funnelled through the small vocabulary above so a
later undo/redo stack wraps it without touching call sites (decision 10).

## Tests (`tests/unit`, device-free)

- Add/remove nodes; generational `NodeId` invalidation after `RemoveNode`.
- `Connect` accepts a valid output→input of compatible type; rejects wrong direction,
  double-booked input, incompatible type (via a stub `CanConnect`), and a cycle.
- `RemoveNode` drops incident links from both endpoints.
- `TopoOrder` returns a valid topological ordering on a small diamond graph; is stable.
- Wildcard pins connect to any value pin.

## Acceptance

`libveng_editor` exposes `VengEditor/NodeGraph/NodeGraph.h`; the topology core builds with no
Vulkan/ImGui/imnodes include; the unit suite covers add/remove/connect/validate/topo and is
green with no ICD; `include_hygiene` green; smoke PPM unchanged. Commit:
`Plan 01: NodeGraph topology core — generic pure graph, validation, named VengEditor surface`.
</content>
