# Plan 04 — Docs + roadmap re-cut

**Goal:** record what landed. Update the engine-facing docs (`CLAUDE.md`), the planset
index (`plans/README.md`), and the future roadmap — mark **area 9 done**, trim
`compiled-rendergraph.md` to its enduring seams, and note in `scene-renderer.md` that
its one enabling prerequisite has shipped. Roadmap-only; no code.

## Why this is its own plan

Every planset closes by reconciling the roadmap with reality so the next phase starts
from an honest map. The compiled graph changes a rule documented in `CLAUDE.md` (the
"RenderGraph: barriers fall out of declared use" section now also describes a compiled
lifecycle and a transient/import resource model), and it discharges a future area —
both must be reflected.

## `CLAUDE.md`

- **The RenderGraph section.** It currently says "Don't hand-write barriers — declare
  a pass with the views it writes/reads; the graph derives the transitions." Keep that
  core truth and add the now-true facts: passes declare **logical resources**
  (`CreateTransient` for graph-owned transients, `Import` for external resources whose
  view is supplied per frame as an `Execute` binding); the callback receives a
  **`PassContext`** (`Cmd()` + `Resolved(ResourceId)`); and `RenderGraph` is a
  **builder** whose **`Compile()`** returns a **`CompiledGraph`** that **replays** the
  baked schedule per frame via `Execute(cmd, imports)`, the consumer re-`Compile()`ing
  only on a structural change (passes added/removed, transient extent/format). State it
  as present-tense fact, no plan citations (per the comment/doc rules).
- Cross-check the hello-triangle pattern reference (`RenderScene`/`CompositeToSwapChain`)
  still reads correctly against the migrated sample (transients/imports, member graphs).

## `plans/README.md`

Add the planset-8 bullet in sequence after planset-7, matching the established voice:
compiled `RenderGraph` (area 9) — resource-model split (graph-owned transients vs.
late-bound imports), `PassContext` record-time channel, `Compile()` → replay with an
explicit dirty-flag recompile seam, one-time validation, and transient aliasing via a
pure unit-tested live-range rule. Note it builds only on the shipped `RenderGraph` and
is the enabling prerequisite for area 8.

## `plans/future/README.md`

- Mark **area 9 (Compiled RenderGraph) DONE — planset-8**, with the one-line summary,
  mirroring how areas 1/2/3/5 are annotated.
- Update the **Ordering & dependencies** section: area 9 moves to the done list; the
  ASCII dependency sketch loses the `9 → enables 8` open edge (9 is now satisfied), so
  area 8 (scene renderer) is gated only on the scene/entity model (area 7) for its
  `Scene`/`Camera` and on area 6 (editor) as its first consumer — no longer on a
  rendering prerequisite.
- Update the **Status** paragraph: area 9 complete; remaining undetailed areas are 4
  (events/input), 6 (editor), 7 (scene/entity), 8 (scene renderer), plus hot-reload.

## `plans/future/compiled-rendergraph.md`

Trim to the enduring vision the way planset-5/6 trimmed their delivered design docs:
add a top note that the area shipped in planset-8 and point at it; keep the parts that
remain future (dead-pass culling, multi-queue/async-compute scheduling, the
parallel-record story from area 2, the immediate-mode-fallback question — now resolved
to "single compiled path") as the seams a later planset may revisit. Remove the
"becomes its own planset when taken up" framing.

## `plans/future/scene-renderer.md`

Update the dependency notes: the **compiled `RenderGraph` prerequisite has landed**
(planset-8). `SceneRenderer` was designed to ship against the immediate-mode graph and
gain the compiled internals for free; that internal benefit now exists, and its
`PassContext::Resolved`/`Compile`/`Resize`/`Configure` seams match what shipped. Its
remaining gates are unchanged: the scene/entity model (area 7) for `Scene`/`Camera`,
and the editor (area 6) as first consumer.

## Acceptance

- All four docs updated; internal cross-links resolve.
- No code change; `ctest` still green from plan 03 (this plan touches only Markdown).
- The future roadmap reads honestly: area 9 done, area 8's enabling prerequisite
  satisfied, no dangling "compiled graph is a later upgrade" references in the
  delivered docs.
- Commit as `planset-8: <summary>` (roadmap-only) with the `Co-Authored-By` trailer.
