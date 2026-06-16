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
  entity inspector uses (including the `AssetHandle` asset picker built in plan 05 — the
  texture story, decision 6), and
- the **serializer** reads/writes them name-keyed and tolerantly.

Mutation routes through `SetProperty(NodeId, const FieldDescriptor&, std::span<const std::byte>)`
(or a typed overload), completing the mutation vocabulary plan 01 opened so the future undo
stack wraps every change (decision 10). A separate **read-only** `PropertyBytes(NodeId) const`
span feeds the serializer and the inspector's draw walk; the writable path is only
`SetProperty`. Node properties are restricted to **builtin leaf `FieldClass`es**
(Scalar/Vector/Quaternion/Enum/AssetHandle) — a node-local nested struct is out of scope, so
the inspector/serializer never needs a `TypeRegistry::Info` lookup for an unregistered type.
This restriction bounds what the *generic* layer permits; it does not promise every class is
editable. The v1 material catalog (plan 03) uses only Scalar/Vector (a `Param`'s value) and
AssetHandle (a `TextureSample`'s texture, edited via the picker plan 05 builds). The inspector's
`Enum` widget remains a read-only label this planset (no value table yet), so a node type that
exposes an `Enum` property would display but not edit — no v1 material node does.

## Serialization

`editor/include/VengEditor/NodeGraph/NodeGraphSerialize.h` — read/write a `NodeGraph` to a
JSON object (not a file; the panel embeds it under `"_editor"` in the `.vmat.json`,
decision 7):

- **Version:** the serialized object carries a `"version"` integer. On read, a version greater
  than the serializer's `NodeGraphFormatVersion` returns a distinguished
  `Result`-error/"read-only" signal so the panel can open without regenerating `fields`
  (decision 8) rather than silently dropping nodes.
- **Nodes:** `NodeTypeId` (by stable catalog name, not the runtime-assigned integer, so ids
  survive a catalog reorder), canvas position, and the property values.
- **Property values:** a **per-`FieldClass` JSON walker** over the node type's
  `FieldDescriptor`s — new code, not the binary `WriteFields`/`ReadFields` (those emit a
  `vector<u8>` blob and require a `TypeInfo` + `TypeRegistry&`, neither of which fits a JSON
  object embedded in a hand-diffable `.vmat.json`). The walker reuses only the
  `FieldDescriptor` layout (offset + `FieldClass`): a `Scalar`/`Vector` writes a JSON number or
  array, an `AssetHandle` writes its `AssetId` decimal with "invalid = none" (rehydrated on
  load), an `Enum` its integer. The builtin-leaf restriction above keeps the walker's switch
  closed and registry-free.
- **Links:** endpoint node + pin name (pins by name, not index, for forward tolerance).
- Tolerant on read **within a supported version:** an unknown node type or a dangling link is
  dropped with a logged warning, not a hard failure. A *newer* version is the one case that
  refuses regeneration (above), since a degraded parse must not overwrite the author's `fields`.

Serialization is JSON-object level only; the panel owns the file I/O and the `.vmat.json`
round-trip (reusing planset-14's preserve-unknown-keys patch).

## Boundary

`NodeType.h` and the serializer headers pull in `FieldDescriptor` (libveng public) and the
JSON type only in the `.cpp` (PRIVATE `nlohmann_json`, as the editor already links it). The
new public headers are added to the `include_hygiene` compile set. No Vulkan/GLFW/imnodes.
`include_hygiene` stays green.

## Tests (`veng_editor_unit`, device-free)

- Register node types into a `NodeCatalog`; `Find` round-trips by id and by name.
- Build a small graph with a node carrying a `{ vec4 Value; }` property; `SetProperty` it;
  serialize to JSON; deserialize into a fresh graph; assert nodes, links, positions, and the
  property value match.
- Round-trip a node with an `AssetHandle` property: the `AssetId` persists and rehydrates;
  an invalid id serializes as "no asset".
- Deserialize a graph referencing an unknown node type → the node is dropped, the rest loads,
  a warning is logged (no crash).
- Deserialize a graph whose `"version"` exceeds `NodeGraphFormatVersion` → the read-only signal
  is returned (no partial graph, no silent regeneration).

## Acceptance

`VengEditor/NodeGraph/` exposes `NodeType`/`NodeCatalog` and the serializer; node properties
walk through the per-`FieldClass` JSON walker and the inspector widgets; the versioned
round-trip unit tests are green device-free in `veng_editor_unit`; `include_hygiene` green;
smoke PPM unchanged. Commit:
`Plan 02: node catalog + instance properties + graph serialization (generic Layer 2)`.
</content>
