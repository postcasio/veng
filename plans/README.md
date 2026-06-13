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
  is deferred to the future-work areas below.

- **[planset-3](planset-3/README.md)** — unit testing & test infrastructure
  (✅ done, 6 plans). Delivered the first half of the future "testing" area (area
  5a): a header-only framework (doctest), CTest wiring, a death-test harness, and a
  base suite — pure-logic, type-mapping round-trips, an extracted barrier-decision
  rule + tests, death tests, and a consolidated GPU one-exe band that skips with no
  ICD. The in-process multi-case GPU integration suite (5b) and CI stay in
  `future/`, deferred until after the de-globalize change.

- **[future](future/README.md)** — work beyond the current plansets (📝 draft/vision,
  holding area; not a planset). The asset system (asset API first, then
  materials/textures/meshes; absorbs offline shader reflection + shader-derived
  layouts), a threading/task system (Vulkan-queue-correct async asset loading),
  de-globalizing the context singleton, the event/input systems, and the
  post-de-globalize GPU integration tests (area 5b). Each becomes its own planset
  when taken up. (Testing area 5a is now planset-3.)
