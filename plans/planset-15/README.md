# planset-15 — node-based material editor (area 6, sub-area C)

**Phase goal:** deliver the node-based material editor — the headline editor slice and the
**loaded `.vmat` path** the asset system left open. An imnodes graph authors a material; its
compiled output is a `.vmat.json` field list; the cook-on-demand + hot-reload loop previews
it live on a sphere. The reusable graph machinery lands as a **named `libveng_editor`
surface** (`VengEditor/NodeGraph/`) so later editors and game editor modules build on it.

This is [future area 6](../future/README.md#6-editor-application) sub-area C, the
prioritized next editor planset, following sub-area B (editor shell + framework, planset-14).

## Prerequisites

All in place:

- **Editor shell + framework** (planset-14): `EditorPanel`, `EditorRegistry`
  (`RegisterAssetEditor` / `RegisterFieldWidget`), `EditorHost`, the asset browser's
  double-click → `AssetEditorFactory` open flow, and `AssetSourceIndex` (AssetId → source).
- **Cook-on-demand** (planset-14): `EditorHost::RequestCook`, `CookSession`,
  `AssetManager::MountMemory`/`MountHandle`. The `MaterialImporter` already cooks
  `*.vmat.json` in `libveng_cook`, so recooking an authored material is already supported by
  the existing backend — no importer change.
- **Reflection layer** (planset-10/11): `FieldDescriptor`/`FieldClass` and the inspector's
  per-`FieldClass` widgets (including the `AssetHandle` asset picker). Node properties reuse
  these wholesale.
- **SceneRenderer + primitives** (planset-7/12/13): `Primitives::Sphere`, `SceneRenderer`,
  `GetOutput`. The live preview renders one sphere with the authored material, exactly the
  `SceneViewportPanel` pattern.
- **imnodes** is fetched (`CMakeLists.txt`, pinned master commit) but **not yet compiled
  into any target** — this planset adds its aggregation TU. The runtime never links it.

## The architecture — four layers, hard dependency ceilings

The design rule that keeps node graphs from rotting: **a lower layer never names a higher
concept.** Topology never says "material"; the model never draws imnodes.

```
Layer 1  NodeGraph topology    pure data + ops + validation. Generic. Unit-tested,
         (named public surface)  device-free, ImGui-free, knows nothing of "material".
              │
Layer 2  Node catalog          data-driven node TYPES (pins + reflected-struct
         (named public surface)  PROPERTIES); node instances = bytes + connections;
              │                   graph (de)serialization. Still generic.
              │
Layer 3  Material specialization  the material node catalog (TextureSample / Param /
         (editor src)             Output / math), the type-compat predicate, and
              │                   CompileMaterialGraph(graph) → .vmat field list +
              │                   the inverse flat-import. The ONLY layer that says
              │                   "material".
              │
Layer 4  MaterialEditorPanel    imnodes canvas (drives Layer 1), node-property
         (editor src)             inspector (reuses Layer-2 field widgets), the live
                                  preview (Layer P), wired to the edit→compile→cook→
                                  hot-reload→preview loop. Owns no model truth.

Layer P  Material preview        a reusable surface: a one-sphere Scene + a SceneRenderer
         (editor src)             → preview RT → ImGuiTexture, SetMaterial each edit.
                                  Depends only on the engine — independent of Layers 1–3.
```

## Decisions

1. **The generic graph is a named `libveng_editor` surface.** `VengEditor/NodeGraph/`
   (`NodeGraph.h`, `NodeType.h`, serialization). It is reusable by future editors and game
   editor modules — not buried as a material-internal seam. The material specialization
   (Layer 3) consumes it from `editor/src/material/` and is **not** part of the public
   surface.

2. **The topology core is pure and imnodes-free.** No ImGui, no Vulkan, and no mention of
   "material" — so it is unit-tested device-free (the `DecideBarrier`/live-range pattern).
   Only the panel (Layer 4) touches imnodes; the model is a pure projection target.

3. **`NodeType` is data, not a subclass.** A node type is a descriptor — value pins (in/out)
   plus a **reflected property struct** (`vector<FieldDescriptor>`). A node *instance* holds
   an opaque byte buffer matching that struct (walked by the existing reflection serializer
   and inspector widgets, exactly like an ECS component) plus its connections. Adding a node
   type is a catalog entry, never a subclass with virtuals.

4. **Pin types reuse the `TypeId` space; pin-type wraps it.** A `PinType` is
   `{ Value(TypeId), Wildcard }` — data pins carry a builtin leaf `TypeId` (`vec4`, `f32`).
   Connection validity is a domain-supplied `CanConnect(PinType, PinType)` predicate (Layer 1
   takes the hook; the material catalog supplies coercion rules — scalar→vector splat, etc.).
   Acyclicity / direction / arity are generic and owned by Layer 1.

5. **`NodeTypeId` is editor-local, not a global `TypeId`.** Node-type identity is a catalog
   id inside `libveng_editor`; node types are not registered into the runtime `TypeRegistry`
   (no global-id pollution, no minting). Pin data types still reference the existing builtin
   `TypeId`s.

6. **Textures are node *properties*, not wired pins.** A `TextureSample` node has a
   `Texture` property of `FieldClass::AssetHandle` (`AssetHandle<Texture>`), drawn by the
   reused asset-picker widget and compiled into a `texture` field of the `.vmat`. Assets
   never travel on a link in v1, so Layer 1 stays asset-agnostic. A future "exposed texture
   parameter" adds an `Asset(TypeId)` pin kind without disturbing v1.

7. **The graph document is embedded in the `.vmat.json`.** The cooked source stays one file
   per asset (the manifest is `{id, type, source}`). The node graph (nodes, canvas
   positions, property values, connections) lives under an editor-only `"_editor"` key; the
   `MaterialImporter` ignores unknown keys, and compile **regenerates** the `fields` array.
   This reuses planset-14's "preserve unknown keys" JSON round-trip — one source of truth.

8. **Opening a flat `.vmat` synthesizes a graph.** A material with no `"_editor"` block
   imports into a default graph (a `TextureSample`→`Output` per texture field, a `Param` per
   value field); the first save embeds the graph. So existing materials open without manual
   migration.

9. **v1 binds parameters; it does not generate shader source.** The graph wires
   textures/params into an author-provided shader's reflected `MaterialData` — reusing the
   constructed-material path wholesale. Node→Slang codegen is deferred; the node/property
   model is shaped so codegen can layer on without breaking v1 graphs.

10. **No undo/redo this planset.** All model mutation routes through the narrow Layer-1
    command vocabulary (`AddNode`/`RemoveNode`/`Connect`/`Disconnect`/`SetProperty`/
    `MoveNode`) so the future command stack is a thin wrapper, not a refactor — but the stack
    itself is out of scope.

## Scope

| In scope | Out of scope |
|---|---|
| `VengEditor/NodeGraph/` named surface: topology core + data-driven catalog + serialization | Node→Slang shader-graph codegen (deferred) |
| Material node catalog (TextureSample, Param/Constant, Output, basic math), type-compat | Wired asset pins / exposed texture parameters |
| `CompileMaterialGraph` (graph → `.vmat` field list) + flat-`.vmat`→graph import | Undo/redo command stack |
| `MaterialEditorPanel`: imnodes canvas, node-property inspector, live preview | Multi-viewport OS windows |
| Reusable material preview surface (sphere + `SceneRenderer` → RT) | Scene editor (sub-area D — its own planset) |
| imnodes aggregation TU compiled into `libveng_editor` (runtime never links it) | New cooked assets / new `AssetId`s (brick.vmat reused) |
| hello-triangle: open `brick.vmat.json` in the node editor, edit, live preview | Custom game-defined node types (the seam exists; no sample) |
| Update `plans/README.md`, `plans/future/README.md`, `CLAUDE.md` | — |

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [NodeGraph topology core](01-nodegraph-core.md) | Layer 1: `NodeGraph`, `NodeId`/`Link`/`PinType`, mutation vocabulary, validation (direction/arity/acyclicity), the `CanConnect` hook. Named public surface. Pure, device-free, unit-tested. | proposed |
| 02 | [Node catalog + serialization](02-node-catalog-serialize.md) | Layer 2: data-driven `NodeType` (pins + reflected property struct), node-instance byte storage, graph (de)serialization to/from a JSON object. Generic. | proposed |
| 03 | [Material catalog + compile](03-material-catalog-compile.md) | Layer 3: material node types, the coercion `CanConnect`, `CompileMaterialGraph` → `.vmat` fields, flat-`.vmat`→graph import. Material-specific, in editor src. | proposed |
| 04 | [Material preview surface](04-material-preview.md) | Layer P (**parallel**): a reusable one-sphere `Scene` + `SceneRenderer` → preview RT → `ImGuiTexture`, `SetMaterial` per edit, re-fetch on hot-reload. Depends only on the engine. | proposed |
| 05 | [MaterialEditorPanel + integration](05-material-editor-panel.md) | Layer 4: imnodes canvas over the graph, property inspector, wire compile→`RequestCook`→hot-reload→preview, register for `AssetType::Material`, imnodes CMake TU, hello-triangle migration. | proposed |
| 06 | [Docs + roadmap re-cut](06-docs-roadmap.md) | `future/editor.md` sub-area C → delivered; `future/README.md` + `plans/README.md` + `CLAUDE.md`. No code. | proposed |

## Dependency analysis & parallel dispatch

The chain is deliberately split so the preview surface (04) runs **concurrently** with the
whole topology→compile chain (01→02→03), since it depends only on the shipped engine.

```
        ┌──────────────────────── critical path ────────────────────────┐
01 core ──► 02 catalog/serialize ──► 03 material compile ──► 05 panel ──► 06 docs
                                                              ▲
04 material preview  ─────────────────────────────────────────┘
  (no dependency on 01–03; depends only on SceneRenderer/Primitives/Material)
```

| Wave | Dispatch | Depends on | Notes |
|---|---|---|---|
| 1 | **01** ‖ **04** | engine only | Two disjoint agents in parallel: topology core and preview surface share no files. |
| 2 | **02** | 01 | 04 may still be in flight. |
| 3 | **03** | 02 | The material specialization. |
| 4 | **05** | 03 **and** 04 | The integration join — needs both branches landed. |
| 5 | **06** | all | Docs, no code. |

- **Parallelizable:** plans **01 and 04** (Wave 1). 04 is a clean, engine-only module
  (`MaterialPreview`) testable against the existing cooked brick material with no node graph
  present, so it parallelizes the entire 01→02→03 chain and hides under the critical path.
- **Strictly sequential:** 01 → 02 → 03 (each builds on the prior layer's types), then
  05 (the join), then 06.
- **Critical path:** 01 → 02 → 03 → 05 → 06 (five plans). With 04 dispatched in Wave 1, the
  preview work costs no extra wall-clock time.
- **File-contention note:** 05 is the only plan that touches `editor/CMakeLists.txt` (imnodes
  TU) and `EditorHost`/`AssetBrowserPanel` wiring; 01–03 add new files under
  `VengEditor/NodeGraph/` and `editor/src/material/`; 04 adds `editor/src/material/MaterialPreview.*`.
  03 and 04 both land under `editor/src/material/` but in disjoint files — safe to run in
  separate worktrees, merged before 05.

## Process & conventions

Same cadence as every planset: implement → migrate `examples/hello-triangle` in the same
pass → verify (clean build, `ctest` green, smoke PPM correct size + exit 0) → update this
table → one commit per plan.

- **`libveng_editor` boundary holds.** The `NodeGraph` public surface pulls in only libveng
  vocabulary + `FieldDescriptor`; no Vulkan, no GLFW, no imnodes in a public header. imnodes
  is a src-only dependency of the panel. `include_hygiene` stays green.
- **The runtime never links imnodes.** The aggregation TU compiles into `libveng_editor`
  only; `libveng` and `libgame` are untouched.
- **The smoke golden is unchanged.** The editor is a separate executable; the launcher PPM
  is unaffected.
- **Contract comments are present-tense facts** — no plan citations, no "future work"
  phrasing (CLAUDE.md).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

veng has a working visual material authoring loop: launch `hello_triangle-editor`,
double-click `brick.vmat`, see its graph (a texture sample feeding the albedo output, a
factors param), wire and tweak nodes, set the albedo texture through the asset picker, and
watch a live-previewed sphere update as the material recooks off-thread — then save, the
graph embedded in the `.vmat.json`, the `fields` array regenerated. The **loaded `.vmat`
path** is closed end-to-end, and the generic `VengEditor/NodeGraph/` surface is in place for
the scene editor (sub-area D) and game editor modules to build on.
</content>
</invoke>
