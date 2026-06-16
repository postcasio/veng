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

## Tests (device-free)

The `NodeGraph` symbols live in `libveng_editor`, which the existing `veng_unit` target does
**not** link (it links `veng::veng` + doctest only). Add a dedicated **`veng_editor_unit`**
test target (in the root `CMakeLists.txt`, beside `veng_unit`) that links
`veng_editor::veng_editor` + doctest, with `editor/src/` on its include path so plan 03's tests
can include the `editor/src/material/` headers — the material sources themselves live in
`libveng_editor` and are *linked*, not recompiled into the test (recompiling would duplicate the
symbols the library already exports). It runs under the same device-free band as `veng_unit`;
plans 01/02/03's device-free tests land there.

Plan 03's one **cook-through** test is the exception: it needs both `CompileMaterialGraph`
(editor) and the real `MaterialImporter` (cooker, which reflects the shader via Slang at cook
time). It lives in the cooker suite (`tests/cooker` / `veng_cooker`), which already links
`libveng_cook` and runs with Slang present; for that test the suite **additionally links
`veng_editor::veng_editor`**. (Linking both libraries into one test exe is fine — they are
independent, no cycle.)

- Add/remove nodes; generational `NodeId` invalidation after `RemoveNode`.
- `Connect` accepts a valid output→input of compatible type; rejects wrong direction,
  double-booked input, incompatible type (via a stub `CanConnect`), and a cycle.
- `RemoveNode` drops incident links from both endpoints.
- `TopoOrder` returns a valid topological ordering on a small diamond graph; is stable.
- Wildcard pins connect to any value pin.

## `include_hygiene` wiring

The new `NodeGraph.cpp` is added to the `libveng_editor` source list in `editor/CMakeLists.txt`.
The existing `veng_include_hygiene` target links only `veng::veng`, so it cannot see the
`VengEditor/` headers. Extend it to also link `veng_editor::veng_editor` — which propagates the
`VengEditor/` include dir and the editor's PUBLIC deps (glm/fmt/ImGui) while keeping imnodes and
nlohmann PRIVATE — and add `#include <VengEditor/NodeGraph/NodeGraph.h>` to
`tests/include_hygiene.cpp` (plan 02 adds its headers there too). A leaked backend include then
fails the hygiene build exactly as it does for libveng headers.

## Acceptance

`libveng_editor` exposes `VengEditor/NodeGraph/NodeGraph.h` (the `NodeGraph.cpp` added to the
`editor/CMakeLists.txt` source list, the header added to `tests/include_hygiene.cpp` with
`veng_include_hygiene` now linking `veng_editor` so a leaked backend include fails the build);
the topology core builds with no Vulkan/ImGui/imnodes include; the `veng_editor_unit` suite
covers add/remove/connect/validate/topo and is green with no ICD; `include_hygiene` green; smoke
PPM unchanged. Commit:
`Plan 01: NodeGraph topology core — generic pure graph, validation, named VengEditor surface`.
</content>
