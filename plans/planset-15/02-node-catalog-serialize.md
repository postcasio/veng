# Plan 02 — node catalog + serialization (Layer 2)

**Goal:** the data-driven node *type* system and node *instance* storage on top of the
topology core, plus graph (de)serialization to a JSON object. Still generic — no "material"
appears here. A node type is a descriptor (pins + a reflected property struct); a node
instance is a byte buffer the existing reflection serializer and inspector widgets walk,
exactly like an ECS component.

## Node types — data, not subclasses (decision 3)

`editor/include/VengEditor/NodeGraph/NodeType.h`:

```cpp
namespace VengEditor
{
    struct NodeTypeId { Veng::u32 Value = 0; };     // editor-local; NOT a runtime TypeId

    struct PinDesc { Veng::string Name; PinType Type; };

    struct NodeType
    {
        NodeTypeId Id;
        Veng::string Name;                          // catalog display name
        Veng::vector<PinDesc> Inputs;
        Veng::vector<PinDesc> Outputs;

        // The node's editable properties as a REFLECTED struct: hand-authored
        // FieldDescriptors over a POD the instance stores as bytes. Drawn by the
        // inspector's per-FieldClass widgets and walked by the reflection
        // serializer — the exact component machinery, reused.
        Veng::vector<Veng::FieldDescriptor> Properties;
        Veng::usize PropertySize = 0;               // sizeof the property struct
    };

    // The catalog a domain (Layer 3) fills. Owns the type descriptors and supplies
    // NodeGraph's PinShapeFn / instance property layout.
    class NodeCatalog
    {
    public:
        NodeTypeId Register(NodeType type);         // mints/holds the descriptor
        [[nodiscard]] const NodeType* Find(NodeTypeId) const;
        [[nodiscard]] std::span<const NodeType> Types() const;   // for the "add node" menu
    };
}
```

`NodeTypeId` is local to the catalog (decision 5) — node types are not registered into the
runtime `TypeRegistry`, so no global ids are minted. Pin data types still reference the
existing builtin leaf `TypeId`s (`vec4`, `f32`).

## Node instances — opaque property bytes

The graph stores, per node, an opaque buffer sized to its type's `PropertySize`. Property
read/write goes through the reflection layer (`FieldDescriptor.Offset` into the buffer), so:

- the **inspector** draws a node's properties with the same per-`FieldClass` widgets the
  entity inspector uses (including the `AssetHandle` asset picker — the texture story,
  decision 6), and
- the **serializer** reads/writes them name-keyed and tolerantly, identical to components.

Add `SetProperty(NodeId, ...)`-style access to the graph (or a `PropertyBytes(NodeId)` span
the reflection walker drives). This completes the mutation vocabulary plan 01 opened.

## Serialization

`editor/include/VengEditor/NodeGraph/NodeGraphSerialize.h` — read/write a `NodeGraph` to a
JSON object (not a file; the panel embeds it under `"_editor"` in the `.vmat.json`,
decision 7):

- **Nodes:** `NodeTypeId` (by stable catalog name, not the runtime-assigned integer, so ids
  survive a catalog reorder), canvas position, and the property struct via the existing
  name-keyed `WriteFields`/`ReadFields` (so an `AssetHandle` property persists its `AssetId`
  with "invalid = none", rehydrated on load — free).
- **Links:** endpoint node + pin name (pins by name, not index, for forward tolerance).
- Tolerant on read: an unknown node type or a dangling link is dropped with a logged warning,
  not a hard failure (a graph authored against a newer catalog still partially opens).

Serialization is JSON-object level only; the panel owns the file I/O and the `.vmat.json`
round-trip (reusing planset-14's preserve-unknown-keys patch).

## Boundary

`NodeType.h` and the serializer headers pull in `FieldDescriptor` (libveng public) and the
JSON type only in the `.cpp` (PRIVATE `nlohmann_json`, as the editor already links it). No
Vulkan/GLFW/imnodes. `include_hygiene` stays green.

## Tests (`tests/unit`, device-free)

- Register node types into a `NodeCatalog`; `Find` round-trips by id and by name.
- Build a small graph with a node carrying a `{ vec4 Value; }` property; set it; serialize to
  JSON; deserialize into a fresh graph; assert nodes, links, positions, and the property value
  match.
- Round-trip a node with an `AssetHandle` property: the `AssetId` persists and rehydrates;
  an invalid id serializes as "no asset".
- Deserialize a graph referencing an unknown node type → the node is dropped, the rest loads,
  a warning is logged (no crash).

## Acceptance

`VengEditor/NodeGraph/` exposes `NodeType`/`NodeCatalog` and the serializer; node properties
walk through the existing reflection widgets/serializer; the round-trip unit tests are green
device-free; `include_hygiene` green; smoke PPM unchanged. Commit:
`Plan 02: node catalog + instance properties + graph serialization (generic Layer 2)`.
</content>
