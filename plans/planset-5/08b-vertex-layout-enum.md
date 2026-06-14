# Plan 08b — Engine-defined vertex layouts as assets

**Goal:** remove per-shader vertex input reflection from the runtime interface.
Vertex layouts become a first-class asset type. The engine ships a **core pack**
with built-in layouts (Canonical, ScreenSpace, PositionOnly); consumers define
additional layouts as assets in their own packs. Shaders reference their layout
by `AssetId`. The cooker validates reflected inputs against the referenced layout
at cook time; the loader performs a lightweight validation at load time. At
runtime, `ShaderInterface` carries only the layout `AssetId` — the actual
`VertexBufferLayout` is loaded like any other asset.

## Motivation

Plan 08 reflected vertex inputs per-shader and serialized them into the cooked
blob. This is over-engineered: in practice, the engine (and game) owns a handful
of fixed vertex layouts, and shaders must conform to one. Per-shader reflection
just duplicates known layouts and adds unnecessary complexity. This hotfix
replaces the per-shader vertex table with an asset reference and makes layouts
extensible by consumers through the existing asset pipeline.

## Vertex layouts as assets

A `VertexLayoutAsset` is a new asset type — a thin wrapper around a
`VertexBufferLayout`. It is authored in JSON, cooked into the archive, and loaded
via `AssetManager` like any other asset.

### JSON source (`.vlayout.json`)

```jsonc
// layouts/canonical.vlayout.json
{
  "elements": [
    { "format": "RGB32Sfloat", "name": "a_Position" },
    { "format": "RGB32Sfloat", "name": "a_Normal" },
    { "format": "RGB32Sfloat", "name": "a_Tangent" },
    { "format": "RG32Sfloat",  "name": "a_UV" }
  ]
}
```

### Core pack

The engine ships a core pack containing the built-in layouts:

| Name | Elements | Stride |
|------|----------|--------|
| **Canonical** | pos(vec3) + normal(vec3) + tangent(vec3) + uv(vec2) | 44B |
| **ScreenSpace** | pos(vec3) + uv(vec2) | 20B |
| **PositionOnly** | pos(vec3) | 12B |

The core pack is mounted automatically by `AssetManager` at initialization,
before any user packs. Its layout AssetIds are random `u64`s (minted via
`vengc generate-id`), same as any other asset — no reserved ranges, no magic
numbers.

The core pack is embedded in the `libveng` binary at build time (the cooked
`.vengpack` is compiled into a C array via a build-time cook + embed step). This
makes the engine fully self-contained: no runtime file lookup, no path
assumptions.

### Extensibility

Consumers define additional layouts as assets in their own packs:

```jsonc
// In user pack JSON:
{ "id": 8837291047, "type": "vertex_layout", "source": "layouts/skinned.vlayout.json" }
```

Their shaders reference these by AssetId exactly as they would a built-in layout.
No registration API, no CLI flags, no config files — just assets.

### Asset provenance

When debugging, knowing which pack an asset came from is useful. Rather than
storing a back-pointer on each asset entry (lifetime concerns if packs are ever
unmounted), the `AssetManager` derives provenance on demand by searching its
mounted packs for the ID. This is a debug/tooling query, not a hot path — no
storage, no lifetime coupling.

## Shader → layout reference

Shaders declare which layout they consume by `AssetId`:

### Slang path (cook from source)

```jsonc
{ "type": "shader", "source": "shaders/brick.slang", "entry": ["vsMain", "fsMain"],
  "vertex_layout": 9182736450128374 }
```

The cooker reflects the Slang shader's vertex inputs, resolves the referenced
layout (from the same pack or a `--reference` pack), and validates they match
element-for-element (format in location order). Mismatch is a **fatal cook
error** with a diagnostic naming the reflected attributes and the expected layout.

If `"vertex_layout"` is omitted, the shader has no vertex stage (compute,
fragment-only) or performs vertex pulling. Both cases result in no vertex input
state at pipeline creation.

### Inline SPIR-V path

```jsonc
{ "type": "shader", "spirv_b64": "…",
  "interface": { "bindings": [...], "push_constants": [...] },
  "vertex_layout": 9182736450128374 }
```

Same validation: the cooker reflects the SPIR-V's vertex inputs and validates
against the referenced layout.

### No vertex inputs

Both "no vertex stage" (compute, fragment-only) and "vertex pulling" (vertex
stage with no inputs) are expressed by **omitting** `"vertex_layout"` from the
shader JSON. Both result in `nullopt` in `ShaderInterface::VertexLayoutId` and no
`VkVertexInputBindingDescription` at pipeline creation. The distinction is
semantic (author intent), not functional.

## `ShaderInterface` changes

```cpp
struct ShaderInterface
{
    vector<ShaderBinding> Bindings;
    vector<ShaderPushConstant> PushConstants;
    optional<AssetId> VertexLayoutId;  // replaces VertexBufferLayout VertexInputs
    // ...
};
```

- `nullopt` — no vertex input state (no vertex stage, or vertex pulling).
- `AssetId` present — references the layout asset this shader consumes.

`ValidateVertexLayout(const VertexBufferLayout&)` is **removed**.

## Cooked blob changes (`assetformat`)

### New: `CookedVertexLayoutHeader`

```cpp
struct CookedVertexLayoutHeader
{
    u32 ElementCount = 0;
    // Followed by ElementCount × CookedVertexLayoutElement
};

struct CookedVertexLayoutElement
{
    u32 Format = 0;  // underlying Renderer::Format
    char Name[k_ShaderNameCapacity] = {};
};
```

### Shader blob

Replace the variable-length `CookedVertexInputAttribute[VertexInputCount]` with
a `u64` in `CookedShaderInterfaceHeader`:

```cpp
struct CookedShaderInterfaceHeader
{
    u32 BindingCount = 0;
    u32 PushConstantCount = 0;
    u64 VertexLayoutAssetId = 0;  // 0 = no vertex inputs (nullopt)
};
```

`CookedVertexInputAttribute` is **deleted**. The blob layout simplifies:
header → bindings → push constants → SPIR-V.

**Note:** add `static_assert(sizeof(CookedShaderInterfaceHeader) == 16)` to
guard against unexpected padding between the `u32` fields and the `u64`.

## Cooker: cross-pack resolution (`--reference`)

The cooker gains a `--reference <pack-source.json>` flag. This makes another
pack's **uncooked sources** (the JSON pack file + the source files it
references) available for cross-asset resolution during cooking. The cooker
parses the referenced pack JSON, builds an ID→source mapping, and can read the
source files to resolve assets by ID.

This is how a user's shader pack references layouts from the core pack: the
tooling passes `--reference path/to/core-pack.json` when invoking `vengc`. The
cooker doesn't auto-load anything — it's the invoking tooling's responsibility to
pass the right references.

**Build-order note:** when both the core pack and a shader pack are cooked in the
same build, the core pack's sources must be available (not necessarily cooked)
before the shader pack is cooked. Since `--reference` reads uncooked sources,
there is **no cooked-before-cooked dependency** — both packs can be cooked in any
order (or in parallel), as long as the source files exist. The build-order
constraint is on source availability, not cook order.

## Load-time validation

The `ShaderLoader` deserializes `VertexLayoutAssetId`. If non-zero, it loads the
referenced `VertexLayoutAsset` via `AssetManager` and asserts it exists (a
missing layout is a fatal load error — catches corrupted blobs or missing core
pack). The actual per-attribute validation already happened at cook time — the
loader trusts it.

## Pipeline creation

`GraphicsPipelineInfo::VertexBufferLayout` remains an
`optional<VertexBufferLayout>`. Pipeline creation resolves the shader's layout
reference:

```cpp
// In material or draw-setup code:
optional<VertexBufferLayout> vbl = std::nullopt;
if (shaderInterface.VertexLayoutId)
{
    auto layout = m_Assets->LoadSync<VertexLayoutAsset>(*shaderInterface.VertexLayoutId).value();
    vbl = layout->GetLayout();
}
```

## AssetId generation (`vengc generate-id`)

A new `vengc generate-id` subcommand, a thin wrapper around a **library
function** in `assetformat`:

```cpp
// assetformat/include/Veng/Asset/AssetId.h (or similar)
// Generates a random u64 AssetId that does not collide with any ID in the
// provided packs. Regenerates internally if a collision occurs.
[[nodiscard]] AssetId GenerateAssetId(std::span<const AssetPack*> packs);
```

The library function takes already-loaded pack instances, collects their existing
IDs, and generates a random u64 that avoids them. Loading/parsing packs is the
caller's responsibility. In practice collisions are astronomically unlikely with
random u64s, but the check makes it impossible to accidentally shadow an existing
asset.

The CLI wrapper:
```sh
vengc generate-id --reference core-pack.json --reference my-pack.json
```

All IDs — including the core pack's — are minted this way. No magic numbers, no
reserved ranges. Consumers (editors, tooling) link `assetformat` directly and
call the library function; Claude uses the CLI.

**On completion, update `CLAUDE.md`** to note: "When you need a new AssetId, run
`vengc generate-id` — never invent one manually."

### Development workflow (bootstrap)

During plan implementation, `vengc generate-id` may not yet be buildable. Use
clearly-fake placeholder IDs (`0xCAFE000000000001`, `0xCAFE000000000002`, etc.)
during development. As a **final step before commit** — once everything builds
and tests pass — replace all placeholders with properly generated IDs via
`vengc generate-id`. The placeholder prefix `0xCAFE` makes un-replaced IDs
impossible to miss in review.

## Work

Organized into parallelizable streams. Streams B and C can proceed concurrently
once A is complete. D requires both B and C. E is the final integration pass.

```
A (foundation) ──► B (cooker) ──────────┐
                │                        ├──► D (core pack + integration) ──► E (verification)
                └──► C (engine) ────────┘
```

### Stream A — Foundation (`assetformat` + tooling)

1. Add `AssetType::VertexLayout` to the asset type enum.
2. Add `CookedVertexLayoutHeader` + `CookedVertexLayoutElement` blob structs.
3. Replace `CookedVertexInputAttribute` + `VertexInputCount` with
   `u64 VertexLayoutAssetId` in `CookedShaderInterfaceHeader`. Delete
   `CookedVertexInputAttribute`. Add `static_assert` on struct sizes.
4. Add `GenerateAssetId(span<const AssetPack*>)` function.
5. Add `vengc generate-id` subcommand (thin wrapper).

### Stream B — Cooker (depends on A)

1. Add `--reference <pack.json>` flag to `vengc cook` with cross-pack source
   resolution (parse referenced pack JSONs, build ID→source mapping).
2. Add `VertexLayoutImporter` (parses `.vlayout.json`, emits cooked blob).
3. Update `ShaderImporter`: reflect vertex inputs → resolve referenced layout
   via pack or `--reference` → validate element-for-element match → store
   `AssetId` in blob. Remove per-attribute serialization.

### Stream C — Engine (depends on A)

1. Add `VertexLayoutAsset` + `VertexLayoutLoader` (loads cooked blob →
   `VertexBufferLayout`). Register in the loader table.
2. Update `ShaderInterface`: replace `VertexBufferLayout VertexInputs` with
   `optional<AssetId> VertexLayoutId`. Remove `ValidateVertexLayout`.
3. Update `ShaderLoader`: deserialize `VertexLayoutAssetId`; if non-zero, load
   the referenced `VertexLayoutAsset` via AssetManager and assert it exists.

### Stream D — Core pack + integration (depends on B and C)

1. Create the built-in layout `.vlayout.json` sources (Canonical, ScreenSpace,
   PositionOnly) + core pack JSON. Use placeholder IDs during development.
2. Wire the build to cook the core pack and embed it into `libveng` (build-time
   `vengc cook` + C array embed).
3. Wire `AssetManager` to mount the embedded core pack at init.
4. Update `Mesh::CanonicalLayout()`: delegate to loading the canonical layout
   asset by the core pack's well-known ID (exposed as a constant), or replace
   with direct use of the constant. Update mesh loader accordingly.

### Stream E — Verification + finalization (depends on D)

1. Update `shader_loader` GPU test (assert loaded interface carries the expected
   `VertexLayoutId`).
2. Update death test (remove `ValidateVertexLayout` death case).
3. Add a vertex-layout cook/load round-trip test.
4. Add a test that cooking a shader with mismatched inputs fails.
5. Migrate hello-triangle: pipeline creation resolves the layout via the
   shader's `VertexLayoutId`.
6. Replace placeholder `0xCAFE…` IDs with real generated IDs via
   `vengc generate-id`.
7. Update `CLAUDE.md` with `vengc generate-id` usage note.

## Dependencies

Depends on plan 08 (done). Independent of plan 09 (materials); 09 consumes
`ShaderInterface::VertexLayoutId` to resolve the layout for pipeline creation.

## Acceptance

- Clean build, `ctest` green.
- `vengc generate-id` produces valid random u64 IDs.
- Core pack mounts automatically; built-in layouts loadable by AssetId.
- A shader cooked from Slang loads and yields a `ShaderInterface` with the
  correct `VertexLayoutId`.
- Cooking a shader whose vertex inputs don't match the referenced layout **fails**
  with a clear diagnostic.
- A consumer-defined layout asset cooks, loads, and is usable by a shader
  (via `--reference`).
- The hello-triangle smoke still writes a correct-sized PPM (unchanged behavior).
- Validation-clean under `VE_DEBUG`.

## Notes

- The core pack pattern generalizes: future built-in assets (default "missing"
  texture, fallback materials, etc.) will live here too.
- `VertexBufferLayout` as a runtime data structure is unchanged — it's still the
  thing `GraphicsPipelineInfo` consumes. The asset is just the authoring +
  loading wrapper around it.
- No ID range reservation. The flat `u64` keyspace works because IDs are
  effectively UUIDs — collisions don't happen in practice, and the minting tool
  checks against existing assets.
- The `--reference` mechanism reads **uncooked sources**, so there is no
  cook-order dependency between packs. Both the core pack and consumer packs can
  be cooked in parallel as long as source files exist.
