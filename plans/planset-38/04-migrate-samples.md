# Plan 04 — migrate the samples onto generated graphs

**Goal:** prove the whole chain end to end by moving the samples off hand-authored fragment shaders
onto **generated graphs**. hello-triangle's `brick.vmat` becomes a graph that generates its fragment
shader (the hand-authored `brick.frag.slang` is deleted); `tonemap.vmat` becomes a generated
**PostProcess** graph (the domain-sink proof); and the template is co-migrated per the working-norms
rule. This is the planset's acceptance test: the maximal and minimal samples both render from
graph-generated shaders. Depends on **Plan 03** (the sample graphs use the expanded catalog).

## Why it is its own plan

Migrating the shipping samples is the real proof and the one place `smoke_golden` may move — so it is
isolated from the mechanism plans, which all hold the golden still. It is also the **co-migration**
the root working-norms require (`hello-triangle` *and* `template` on every breaking change), and
doing it last means the catalog (Plan 03) is wide enough to express the brick material's actual
shading rather than forcing node additions mid-migration.

## What lands

- **`brick.vmat` → a generated Surface graph.** Author the node graph reproducing the current
  `brick.frag.slang` shading — base color sampled and tinted (`TextureSample` × `BaseColorFactor`),
  packed ORM sampled and factored (occlusion/roughness/metallic), tangent-space normal perturbation,
  emissive strength — wired into the Surface `MaterialOutput`'s Albedo/Normal/ORM sinks. Compile emits
  `brick.gen.frag.{slang,shader.json}` (the sub-asset, Plan 02); **`brick.frag.slang` is deleted** and
  the manifest carries the generated shader.

- **`tonemap.vmat` → a generated PostProcess graph.** The core tonemap material becomes a generated
  PostProcess graph: the HDR input (a runtime-bound texture handle) through the exposure/tonemap
  expression into the single `Color` sink. This is the **PostProcess domain-sink proof** — the emitter
  generating a `float4 … : SV_Target0` entry from the domain contract, the authorable `Exposure`
  exposed param surviving as a per-frame knob. (Tonemap stays the authorable-material exemplar; the
  fixed plumbing composites/debug blits remain hardcoded engine passes, unchanged.)

- **The template co-migrated.** The minimal sample's material (`flat.vmat`/`flat.frag.slang`) follows
  the same path — either migrated to a trivial generated graph or, if the minimal app is clearer with a
  hand-authored shader, kept hand-authored but confirmed building against Plan 00's `#include
  "Veng/material.slang"`. Whichever keeps the template the smallest correct copy-to-start app; the
  decision is recorded in the plan's commit.

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
2. **`brick` proves Surface; `tonemap` proves PostProcess.** The two domains' sinks (`GBufferOutput`
   MRT vs. single `SV_Target0`) are both exercised by a real shipping material, not just a fixture.
3. **Image-preserving where possible; regenerate once if not.** A pure refactor of *how* a shader is
   authored should not change the picture; the plan targets a byte-identical golden, and treats a
   move as a deliberate, documented one-time regeneration — never a silent golden churn.
4. **Co-migrate the template, smallest-correct-app first.** The template's job is to be the minimal
   conformance surface; its material is migrated only insofar as that keeps it minimal and correct.
5. **Hand-authored fragment shaders for migrated materials are deleted.** Once a material generates
   its fragment, the hand-authored `.slang` is dead — leaving it would be two sources of truth.

## Files

| File | Change |
|---|---|
| `examples/hello-triangle/assets/materials/brick.vmat.json` | The authored node graph (under `"_editor"`); `shaders.fragment` = the derived generated id; the generated field list. |
| `examples/hello-triangle/assets/shaders/brick.frag.slang` | **Deleted**; replaced by the generated `brick.gen.frag.{slang,shader.json}` sub-asset. |
| `examples/hello-triangle/assets/` (manifest) | The generated-shader row(s) for `brick` (and `tonemap` if cooked into a sample pack). |
| `engine/assets/core/materials/tonemap.vmat.json` + core pack | The generated PostProcess graph; the generated tonemap fragment sub-asset; `tonemap.frag.slang` retired if fully generated. |
| `examples/template/assets/...` | Co-migration per the recorded decision (generated trivial graph, or confirmed-building hand-authored). |
| `tests/golden/hello_triangle_scene.png` | Regenerated **only if** the migration moves the capture (documented in the commit). |
| `engine/CLAUDE.md`, `examples/*/...` | Note that the samples' materials are graph-generated (the hand-authored-fragment framing retires). |

## Verification

- Clean build; `ctest` green. The launcher smoke + `hello_triangle_launcher_smoke` pass; the migrated
  `brick`/`tonemap` materials cook (offline) and render.
- `smoke_golden`: **unchanged** if the migration is image-preserving; otherwise regenerated **once**
  with the move called out — a green golden after regeneration, and the validation gate clean under
  `VE_DEBUG`.
- The template still compiles and runs (the minimal conformance check), rendering its cube.
- Opening `brick.vmat`/`tonemap.vmat` in the editor shows the authored graph, edits live-recook the
  generated shader, and `MaterialPreview` updates — the full authoring loop on a shipping material.
- The relocatable set still resolves (generated sub-asset cooked into the pack beside the launcher).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
