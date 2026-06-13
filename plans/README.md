# veng plans

Plans are grouped into numbered **plansets**, each a coherent phase of work.

- **[planset-1](planset-1/README.md)** — API rework / insulation (✅ complete,
  2026-06). Ownership, error handling, deferred destruction, vocabulary enums,
  public/backend split, render graph, ImGui module, headless context, typed
  buffers. (Plan 12, runtime shader reflection, was intentionally *not* built —
  superseded by planset-2, see below.)

- **[planset-2](planset-2/README.md)** — rendering API surface cleanup (🚧 in
  progress). Push-constant layout/buffer, attachment formats from render targets,
  retiring the legacy render-pass path, and minor ergonomics. Shader-facing work
  is deferred to planset-3.

- **[planset-3](planset-3/README.md)** — future work (📝 draft/vision, holding
  area). The material system (primary rendering interface; absorbs offline shader
  reflection + shader-derived layouts), de-globalizing the context singleton, the
  event/input systems, and unit-test infrastructure. Will be split into several
  dedicated plansets as each is taken up. Not scheduled — planset-2 first.
