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

Shaders are compiled by `glslc` into the build tree and loaded from an absolute
path baked in at compile time, so the binary runs from any working directory.

Smoke-test mode: set `HT_SMOKE=<path.ppm>` and the app renders 20 frames, dumps
the scene image to that path as a binary PPM, and exits 0. Useful for verifying
a rendering change without eyeballing the window:

```sh
HT_SMOKE=/tmp/scene.ppm ./build/examples/hello-triangle/hello_triangle
```

Known limitation: the window is non-resizable for now — the composite descriptor
set references the ImGui image, which is recreated on swapchain invalidation,
and the sample doesn't re-register it yet. Revisit after plan 04/05.
