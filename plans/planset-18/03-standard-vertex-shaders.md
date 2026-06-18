# Plan 03 — standard engine vertex shaders

**Goal:** a domain implies its vertex stage, so the engine **ships the standard vertex
shader for each domain in the core pack** rather than each game hand-writing one.
`PostProcess` already has one — the core
[fullscreen.vert](../../engine/assets/core/shaders/fullscreen.vert.shader.json). This plan
adds `Surface`'s — promoting the generic deferred vertex stage the example ships as
`brick.vert` into the core pack — and migrates the example material to the engine asset,
deleting its hand-shipped vertex shader.

## Why this is a clean promotion

The example's
[brick.vert.slang](../../examples/hello-triangle/assets/shaders/brick.vert.slang) is
**entirely generic** — there is nothing brick- or example-specific in it. It declares the
canonical-layout `VSInput` (position/normal/tangent/uv), transforms position by the
push-constant MVP, carries the normal to world space by the NormalMatrix, and passes UV +
world normal to the fragment stage. It is exactly the deferred opaque **surface** vertex
contract, written once. Its supporting
[material_data.slang](../../examples/hello-triangle/assets/shaders/material_data.slang)
include is likewise engine-standard — after plan 00's storage fold it declares the unified
material block load (`LoadMaterialParams`), the fixed `GBufferOutput`, the shared 128-byte
push-constant block, and the set-0 bindless arrays at the `BindlessRegistry` layout. Both
belong to the engine, not the sample.

## What lands

### `surface.vert` in the core pack

Add to [engine/assets/core/](../../engine/assets/core/):

- `shaders/surface.vert.slang` — the promoted generic deferred surface vertex stage (the
  current `brick.vert.slang` body), `#include`-ing a core support header.
- `shaders/material.slang` — the engine-standard bindless/material declarations (the
  current `material_data.slang`, post plan-00 fold: the unified-block load
  `LoadMaterialParams`, `GBufferOutput`, the shared `PushConstants` block, the set-0
  bindings, `MaterialParamStride`). The block's *layout* is **not** fixed here — a shader
  declares its own combined `MaterialParams` struct (handles + params); see decision 2. The
  surface vertex stage and a game's surface fragment shader both include it.
- `shaders/surface.vert.shader.json` — `{ "source": "surface.vert.slang", "entry":
  "vsMain", "vertex_layout": 5603155022528551788 }` (the existing core **canonical** layout
  id).
- A new entry in [core.vengpack.json](../../engine/assets/core/core.vengpack.json):
  `{ "id": <surface.vert id>, "type": "shader", "source": "shaders/surface.vert.shader.json" }`.

The `<surface.vert id>` is a marked placeholder during implementation, minted with `vengc
generate-id --reference engine/assets/core/core.vengpack.json` in the planset's final pass
and written as uppercase hex in any C++ constant, decimal in the pack JSON.

### The example migrates to the engine asset

- [brick.vmat.json](../../examples/hello-triangle/assets/materials/brick.vmat.json):
  `"shaders": { "vertex": <core surface.vert id>, "fragment": 1005 }` — the vertex id moves
  from the example-local `1004` to the core asset (already resolvable: the example pack
  already lists the core pack as a `REFERENCE`, which is how `brick.vert` resolves the core
  canonical-layout id today). The new `"domain": "surface"` key from plan 02 is added here
  explicitly.
- Delete `examples/hello-triangle/assets/shaders/brick.vert.slang` and
  `brick.vert.shader.json`, and the `{ "id": 1004, … }` entry from the example pack
  manifest.
- The example's `brick.frag.slang` is unchanged — it keeps shading into the g-buffer. It
  continues to `#include "material_data.slang"` from its own asset dir (deduplicating the
  example's fragment-side include against the new core `material.slang` is a noted possible
  follow-on; cross-pack Slang include paths are out of scope here).

## Decisions

1. **`fullscreen.vert` is already the PostProcess standard — no new asset.** It is the VS of
   every engine fullscreen pass and is in the core pack
   (`17612966569144354344`). The PostProcess pipeline path (plan 04) references it. This
   plan only adds the **Surface** standard; the table is then complete.

2. **`material.slang` declares the shared bindless surface contract, not a fixed block
   layout.** After plan 00 there is one unified material block whose layout is
   **per-material** — the shader declares its own combined `MaterialParams` struct (handle
   `uint`s + authored params; `brick.frag` declares `{ uint Albedo; uint AlbedoSampler;
   float4 Factors; }`). So the core `material.slang` carries the engine-fixed pieces
   (`GBufferOutput`, the push block, the set-0 bindings, `MaterialParamStride`,
   `LoadMaterialParams`), and a game's fragment shader declares its own `MaterialParams`
   before the `Load`. The surface **vertex** stage needs none of `MaterialParams` — it reads
   only the push block — so `surface.vert` is unaffected by where `MaterialParams` is
   declared.

3. **The promotion is byte-identical, so `smoke_golden` is unchanged.** The promoted
   `surface.vert.slang` compiles to the same SPIR-V the example's `brick.vert` does (same
   source, same entry, same canonical layout id). The cooked example material now points its
   vertex id at the core asset, but the resolved shader bytes are identical — the render is
   pixel-for-pixel the same. No golden regeneration.

4. **The example keeps its own `brick.frag` + `material_data.slang`.** Only the **vertex**
   stage is engine-standard; the fragment stage is the material's own shading. Whether the
   example's fragment-side `material_data.slang` is later replaced by an include of the core
   `material.slang` is a cross-pack-include question deferred to a cleanup follow-on — this
   plan does not change the example's fragment compile. The two copies (core `material.slang`
   and the example's `material_data.slang`) must stay byte-identical until that cleanup; each
   carries a cross-comment naming the other so a drift is caught by review, since no test
   guards it (the engine-block drift guard was removed with the fixed struct in plan 00).

## Files

| File | Change |
|---|---|
| `engine/assets/core/shaders/surface.vert.slang` | New — promoted generic surface vertex stage. |
| `engine/assets/core/shaders/material.slang` | New — engine-standard bindless/material/push-block declarations. |
| `engine/assets/core/shaders/surface.vert.shader.json` | New — source/entry + canonical vertex-layout id. |
| `engine/assets/core/core.vengpack.json` | New `surface.vert` shader entry (placeholder id → minted). |
| `examples/hello-triangle/assets/materials/brick.vmat.json` | `vertex` → core `surface.vert` id; `"domain": "surface"`. |
| `examples/hello-triangle/assets/*.vengpack.json` | Remove the `1004` shader entry. |
| `examples/hello-triangle/assets/shaders/brick.vert.{slang,shader.json}` | Deleted. |

## Verification

- Clean build; the core pack cooks with the new `surface.vert` (the cook resolves its
  canonical layout id from within the core pack).
- The example pack cooks: `brick.vmat` resolves its vertex id against the referenced core
  pack; the removed `1004` entry leaves no dangling reference (the manifest no longer lists
  it and the material no longer points at it).
- `vengc verify` on both packs passes (TOC digest + blob hashes consistent after re-cook).
- Smoke PPM correct size + exit 0; **`smoke_golden` unchanged** — the resolved vertex SPIR-V
  is identical (decision 3).
- The relocatable trio still runs (launcher + lib + example pack + core pack beside it).
