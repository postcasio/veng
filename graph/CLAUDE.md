# libveng_graph — the node-graph & material-codegen library

`graph/` is `libveng_graph` (`veng::graph`, headers under `VengGraph/`, namespace `VengGraph`):
the generic node-graph topology core, the material node catalog, and the emit walk that turns a
material graph into generated Slang fragment source. It is linked PUBLIC by **both**
`libveng_editor` and `libveng_cook`, so the editor's live preview and the offline cook run the
**identical** walk by construction. Project-wide conventions live in the
[root CLAUDE.md](../CLAUDE.md); the editor's node-graph UI is in
[editor/CLAUDE.md](../editor/CLAUDE.md) and the cooker's graph-sourced shader path in
[cooker/CLAUDE.md](../cooker/CLAUDE.md).

## Linkage

Links `veng::veng` PUBLIC and `nlohmann/json` PRIVATE; `libveng_editor` and `libveng_cook` link
it PUBLIC (`veng::graph → veng::veng`; `editor → graph`; `cooker → graph` — no cycle). The
library is **ImGui-free and Vulkan-free**: imnodes (the canvas UI) is an editor-only dependency
that never appears here, and the public surface names no JSON-library type (serialization takes
and returns a JSON *document string*). It builds unconditionally from a source build, like
`vengc` — veng *is* the tools.

## The topology core

`NodeGraph` / `NodeType` / `NodeGraphSerialize` are the generic, device-free core:

- **`NodeGraph`** — generational `NodeId`, typed `PinType` pins, the mutation vocabulary, and
  direction/arity/acyclicity validation, parameterized by a construction-time hook set
  (`CanConnectFn` / `PinShapeFn` / `PropertySizeFn`).
- **Node types are data, not subclasses.** A `NodeType` is pins (typed in/out) plus a reflected
  property struct; a node instance stores property bytes the reflection serializer and inspector
  widgets walk, exactly like an ECS component. `NodeTypeId` is graph-local (defined in
  `VengGraph/NodeType.h`), distinct from the runtime `TypeId` space; pin data types reuse builtin
  leaf `TypeId`s. Types live in a data-driven `NodeCatalog`.
- **(De)serialization** round-trips a graph (nodes, positions, property values, links) to/from a
  JSON document string.

## The material emit walk

Every material node is an **expression emitter**: `CompileMaterialGraph` topologically walks the
graph from `MaterialOutput` into generated Slang fragment text. The walk threads a thin typed
**`EmittedValue` `{ Expr; PinType Type; bool IsConst }`** (the code-chunk model — not a parsed
AST, since the graph already *is* the typed acyclic DAG), assigns **one SSA temp per output
pin**, and substitutes downstream applying the link-recorded coercion (`f32→vecN` splat,
`vec4→vec3/vec2` truncate, via `CoerceExpr`). A value used more than once is a temp (a shared
`TextureSample` samples once); a single-use value inlines (so the output is a pure function of
the graph), and an unreached node never emits (free dead-code elimination). Temp/field names
derive from a stable creation-order node key, so the same graph emits **byte-identical** text
across two walks and a save/load round-trip.

Each node type's emit-fn lives in a **`MaterialEmitTable`** keyed by `NodeTypeId`, populated by
`RegisterMaterialNodeTypes` beside minting the types; the topology core stays generic and
emit-free. The catalog is **fixed and schema-independent**
(`RegisterMaterialNodeTypes(catalog, emit, domain)` — keyed by domain only, shaped by pin leaf
types, never by a loaded shader's reflected fields). It is a **shading-expression set**, not
three binding nodes:

- **Bindings:** `TextureSample` (UV input + color output) and `Param` (sized by its property).
- **Sources:** `Constant` (an authored literal of a chosen leaf type, always inlined — its
  `MaterialLeafType` property names the Slang type) and `ScalarParam` (a `Param` specialized to
  `float`, carrying the same provenance).
- **Arithmetic:** `Multiply` / `Add` / `Subtract` / `Divide` (binary, component-wise, a scalar
  splatting against a vector via the link coercion).
- **Shaping:** `Lerp` / `Saturate` / `Clamp` / `OneMinus`.
- **Vector algebra:** `Dot` / `Cross` / `Normalize` / `Length` (the output pin type follows the
  operation — `Dot`/`Length` reduce to `float`, `Cross` is vec3-only by its pin types).
- **Channel plumbing:** `Split` (a vec4 fanned to four named scalar pins via swizzles) /
  `Combine` (four scalar inputs packed into a vec4, an unconnected slot defaulting to 0).

The math/swizzle/utility set is domain-independent (`RegisterMathNodeTypes`, called for every
domain). Textures stay node **properties** (`FieldClass::AssetHandle`), not wired pins, so the
topology core stays asset-agnostic.

**`MaterialOutput`'s sinks are the domain contract.** It emits the domain entry point
(`GBufferOutput fsMain` for Surface, `float4 fsMain … : SV_Target0` for PostProcess) with defined
defaults for unconnected sinks (Surface: Albedo `float4(0,0,0,1)`, Normal the geometric
`input.v_WorldNormal`, ORM `float3(1,1,0)`, Velocity always `ComputeMotionVector(...)`); the
source is prefixed with its domain's contract include (`Veng/surface.slang` or
`Veng/postprocess.slang`).

**A `Param` carries one of three provenances:** *const* folds its value inline; *exposed*
contributes an author-tweakable `MaterialParams` field with a default; *engine-bound* contributes
a field the engine writes by name at runtime (no default, not an instance-override surface). The
walk generates the `MaterialParams` struct — ordered large-alignment-first so the cooker's std140
reflection and the shader's scalar-layout `Load<T>` resolve identical offsets — and the matching
`.vmat` field list from the same pass, so reflected offsets and packed values agree.

## Consumers

- **The cooker** compiles a `*.shader.json` whose source is a `*.graph.json` by walking it into
  Slang text, then compiling/reflecting through the same SPIR-V path a hand-authored `.slang`
  takes — behind a resolver hook so the walk is never statically linked into the importers. See
  [cooker/CLAUDE.md](../cooker/CLAUDE.md).
- **The editor** drives the imnodes canvas + node-property inspector over the same catalog and
  routes its cook-on-demand through the same resolver, so editing a graph regenerates, recompiles,
  and hot-reloads the identical text the offline cook produces. See
  [editor/CLAUDE.md](../editor/CLAUDE.md).
