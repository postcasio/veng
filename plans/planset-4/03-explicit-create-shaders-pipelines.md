# Plan 03 — Explicit context in `Create` — shaders, pipelines, descriptors

**Goal:** finish the public `Create` sweep for the remaining app-facing families.
After this plan, **no app-facing factory reaches `Context::Instance()`** — only the
context-internal primitives (plan 04) still do.

## Scope: this plan's families

- `Shader` (`Shader.h` / `Backend/Shader.cpp`) — both overloads: the
  `Result`-returning `Create(const ShaderInfo&)` (loads from file) and
  `Create(const ShaderBinaryInfo&)`.
- `DescriptorSetLayout` (`Backend/DescriptorSetLayout.cpp`)
- `DescriptorSet` (`Backend/DescriptorSet.cpp`) — incl. the four `Write` overloads
  routed through `m_Context` in plan 01; their `Create` now takes the context too.
- `PipelineLayout` (`Backend/PipelineLayout.cpp`)
- `GraphicsPipeline` (`Backend/GraphicsPipeline.cpp`)
- `ComputePipeline` (`Backend/ComputePipeline.cpp`)

## Work

1. **Change each factory signature** to `Create(Context&, const XInfo&)` (and the
   private constructor to match), capturing `m_Context(context)` — same pattern as
   plan 02. For `Shader::Create(const ShaderInfo&)` the return stays
   `Result<Ref<Shader>>`; thread the context in front of the info and keep the
   file-missing error path intact (`VE_ASSERT` on it at call sites is unchanged).

2. **`DescriptorSet` pool access.** `DescriptorSet::Create` reads the descriptor
   pool via `Context::Instance().GetNative().DescriptorPool`; switch it to the
   passed `context`. (The pool object itself is context-internal and de-globalized
   in plan 04 — here we only stop *reaching it through the global*.)

3. **Migrate `examples/hello-triangle` and `tests/`.** Update all call sites for
   these families. `compute_dispatch` exercises `ComputePipeline` + `DescriptorSet`
   + storage images; `headless_smoke` and the triangle exercise the graphics
   pipeline + sampled-image descriptors — thread the local/app context into each.

## Dependencies

Needs plan 01. Shares the sample file (`main.cpp`) with plan 02 — **sequence the
merge after 02** (or vice-versa), never concurrently.

## Acceptance

- Clean build, `ctest` green, smoke binary writes a correct-sized PPM.
- `grep -rn "Context::Instance()" src/` now matches **only** the context-internal
  primitives and the singleton machinery (`s_Instance`, `Instance()` itself) —
  every app-facing `Create` is explicit. This is the precondition plan 04 needs to
  delete the singleton.
- Validation check under `VE_DEBUG`: `compute_dispatch` (the cross-stage
  barrier + descriptor path) and `headless_smoke` show no new validation ERRORs.
  The known storage-image descriptor gap (CLAUDE.md) is unchanged — not fixed, not
  widened, here.

## Notes

- Same parameter order as plan 02: `Context&` first.
- `GraphicsPipeline::Create` derives its attachment formats from the render-graph
  pass (planset-2 plan 02) — that's orthogonal; just add the context parameter,
  don't refold the format derivation.
- Do not collapse `DescriptorSetLayout` and `PipelineLayout` creation into the
  context or a cache here — that's descriptor/material-phase work
  ([bindless-descriptors](../future/bindless-descriptors.md)). This plan only makes
  the existing creation explicit.
