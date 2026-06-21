# hello-triangle

The reference consumer for veng. A spinning triangle is rendered into an
offscreen scene image, shown inside an ImGui window (via `ImGuiTexture`), and a
fullscreen composite pass blends the scene image and the ImGui output into the
swapchain image.

It deliberately exercises the API surface the `plans/` rework touches: window +
application lifecycle, shader loading, vertex buffers + layout, push constants,
dynamic graphics pipelines, descriptor set layout/creation/update, samplers,
image views over engine-owned images (`Context::GetImGuiImage`), manual image
barriers, and the ImGui frame path. When a plan lands a breaking change, this
sample gets migrated in the same change — if it stops compiling or rendering,
the plan isn't done.

Build (from the repo root):

```sh
cmake -S . -B build
cmake --build build --target hello_triangle
./build/examples/hello-triangle/hello_triangle
```

The scene geometry is a UV sphere built at runtime from a `Primitive` component and
streamed in via `AssetManager::Build<Mesh>` — no cooked mesh is loaded to put it on screen. It
carries the brick material instance on its submesh and is drawn through the same
pipeline a cooked mesh would use.

Assets — the brick material + texture, and every shader (the material's two
stages and the composite pass) — are authored as a JSON pack under `assets/`,
cooked to a `.vengpack` by `vengc` at build time, and loaded at runtime by
`AssetId`. Shaders are written in Slang and compiled to SPIR-V during the cook.
The cooked pack's path is baked in at compile time, so the binary runs from any
working directory.

Smoke-test mode: set `HT_SMOKE=<path.ppm>` and the app renders 20 frames at a
fixed pose, dumps the scene image to that path as a binary PPM, and exits 0. The
fixed pose makes the capture reproducible run to run, so the `smoke_golden` ctest
fuzzy-compares it against `tests/golden/hello_triangle_scene.png`. Useful for
verifying a rendering change without eyeballing the window:

```sh
HT_SMOKE=/tmp/scene.ppm ./build/examples/hello-triangle/hello_triangle
```

Known limitation: the window is non-resizable — the composite descriptor
set references the ImGui image, which is recreated on swapchain invalidation,
and the sample doesn't re-register it.
