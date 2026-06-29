# Plan 04 — migrate the samples onto graph-sourced shaders

**Goal:** prove the whole chain end to end by moving the samples off hand-authored fragment shaders
onto **graph-sourced** ones. hello-triangle's `brick` fragment shader's source becomes a graph (the
hand-authored `brick.frag.slang` is deleted); the core `tonemap` fragment becomes a generated
**PostProcess** graph (the domain-sink proof); and the template is co-migrated per the working-norms
rule. This is the planset's acceptance test: the maximal and minimal samples both render from
graph-generated shaders. Depends on **Plan 03** (the sample graphs use the expanded catalog).

## Why it is its own plan

Migrating the shipping samples is the real proof and the one place `smoke_golden` may move — so it is
isolated from the mechanism plans, which all hold the golden still. It is also the **co-migration**
the root working-norms require (`hello-triangle` *and* `template` on every breaking change), and
doing it last means the catalog (Plan 03) is wide enough to express the brick material's actual
shading rather than forcing node additions mid-migration.

## The migration is a source swap, not a new asset

The `brick` material already references fragment shader id `1005` (`brick.frag.shader.json` →
`brick.frag.slang`). Migration changes shader `1005`'s **uncooked source** from the `.slang` to a
graph, deletes the dead `.slang`, and leaves the material's `shaders.fragment: 1005` reference and the
manifest id untouched — **no new asset, no minted id** (the README/Plan 02 model). The cooker
generates `1005`'s SPIR-V from the graph; nothing else moves.

## What lands

- **`brick`'s fragment shader → a generated Surface graph.** `brick.frag.shader.json`'s source
  becomes `brick.frag.graph.json` (the authored node graph reproducing the current `brick.frag.slang`
  shading — base color sampled and tinted via an exposed `BaseColorFactor`, packed ORM sampled and
  factored, tangent-space normal perturbation, emissive strength — wired into the Surface
  `MaterialOutput`'s Albedo/Normal/ORM sinks). **`brick.frag.slang` is deleted.** Shader `1005` and
  the material's reference are unchanged; the cooker emits `1005` from the graph.

- **`tonemap`'s fragment shader → a generated PostProcess graph.** The core tonemap shader's source
  becomes a PostProcess graph: the HDR input (an **engine-bound** texture handle, Plan 02) through the
  exposure/tonemap expression into the single `Color` sink, with `RenderScale` as an **engine-bound**
  `vec4` field (the renderer writes it each frame) and `Exposure` as an **exposed** param (an
  authorable per-frame knob). This is the **PostProcess domain-sink proof** — the emitter generating a
  `float4 … : SV_Target0` entry reading `g_PC.MaterialIndex` from the PostProcess push block (Plan 00),
  and the proof that engine-bound provenance carries the fields the const/exposed split alone cannot.
  (Tonemap stays the authorable-material exemplar; the fixed plumbing composites/debug blits remain
  hardcoded engine passes, unchanged.)

- **The template co-migrated to a generated graph.** The minimal sample's `cube`/`flat` fragment
  shader's source becomes a trivial generated Surface graph, so the minimal app **exercises codegen**
  (the minimal conformance surface must cover the new authoring path — a hand-authored fallback would
  leave codegen untested at minimal scale). It stays the smallest correct copy-to-start app.

- **Goldens regenerated if they move.** The brick migration aims to be **image-preserving by
  construction** (the generated shader computes the same shading), so ideally `smoke_golden` does not
  move. If the generated Slang differs enough to shift a pixel (e.g. expression ordering changing
  floating-point rounding), the golden is regenerated **once** per the documented procedure
  (`HT_SMOKE` capture → `sips` → `tests/golden/hello_triangle_scene.png`) and the move is called out in
  the commit.

## Decisions

1. **The samples are the acceptance test.** Codegen is not "done" until the shipping samples render
   from generated graphs — a test fixture proves the mechanism, but the migrated samples prove it is
   the real authoring path. This is why migration is its own terminal plan.
2. **`brick` proves Surface; `tonemap` proves PostProcess (and engine-bound).** The two domains' sinks
   (`GBufferOutput` MRT vs. single `SV_Target0`) are both exercised by a real shipping material, and
   tonemap exercises engine-bound provenance, not just a fixture.
3. **Image-preserving where possible; regenerate once if not.** A pure refactor of *how* a shader is
   authored should not change the picture; the plan targets a byte-identical golden, and treats a
   move as a deliberate, documented one-time regeneration — never a silent golden churn.
4. **Co-migrate the template to a generated graph.** The template is the minimal conformance surface,
   so it must exercise codegen — a trivial generated graph, not a kept-hand-authored shader.
5. **Migration is a source swap.** The fragment shader keeps its id and the material keeps its
   reference; only the shader's uncooked source changes (`.slang` → graph) and the dead `.slang` is
   deleted. Leaving the `.slang` would be two sources of truth.

## Files

| File | Change |
|---|---|
| `examples/hello-triangle/assets/shaders/brick.frag.graph.json` | New — the authored Surface node graph (the shader's source). |
| `examples/hello-triangle/assets/shaders/brick.frag.shader.json` | `source` → `brick.frag.graph.json` (id `1005` unchanged). |
| `examples/hello-triangle/assets/shaders/brick.frag.slang` | **Deleted.** |
| `engine/assets/core/shaders/tonemap.frag.graph.json` + `tonemap.frag.shader.json` | New PostProcess graph; the shader entry's source repointed; `tonemap.frag.slang` deleted. |
| `examples/template/assets/shaders/...` | The `flat`/`cube` fragment shader's source becomes a trivial generated graph; the `.slang` deleted. |
| `tests/golden/hello_triangle_scene.png` | Regenerated **only if** the migration moves the capture (documented in the commit). |
| `engine/CLAUDE.md`, `examples/*/...` | Note the samples' fragment shaders are graph-sourced (the hand-authored-fragment framing retires). |

## Verification

- Clean build; `ctest` green. The launcher smoke + `hello_triangle_launcher_smoke` pass; the migrated
  `brick`/`tonemap` shaders cook (offline, from their graphs) and render.
- `smoke_golden`: **unchanged** if the migration is image-preserving; otherwise regenerated **once**
  with the move called out — a green golden after regeneration, and the validation gate clean under
  `VE_DEBUG`.
- The template still compiles and runs (the minimal conformance check), rendering its cube from a
  generated graph.
- Opening `brick.vmat`/`tonemap.vmat` in the editor shows the authored graph, edits live-recook the
  generated shader, and `MaterialPreview` updates — the full authoring loop on a shipping material.
- The relocatable set still resolves (the graph-sourced shader cooks into the pack beside the
  launcher).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
