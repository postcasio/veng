# Plan 02 — screenshot + render-inspection tools

**Goal:** let an agent *see* what the engine is rendering — capture a viewport to a PNG image
returned as an MCP image content block — and read render diagnostics (which viewports exist, cull
counts, GPU frame time). Depends on Plan 01 (the `McpHost` seam, which this plan's `Viewport`
provider populates). Independent of Plan 03.

## The starting point

- The smoke capture is the exact path: `Viewport::GetOutput()->GetImage()->Download()` returns the
  viewport's output as a host `vector<u8>`; the output is **RGBA16F**
  ([examples/hello-triangle/main.cpp](../../examples/hello-triangle/main.cpp) `WriteSceneCapture`
  decodes it to 8-bit RGB via `glm::unpackHalf1x16`). `Image::Download()` is public engine API
  (`Veng/Renderer/Image.h`), blocking until complete.
- A `Viewport` (`Veng/Renderer/Viewport.h`) owns a `SceneRenderer` and produces a sampleable output
  each frame; `GetOutput()` returns the `Ref<ImageView>`, `GetImage()` the backing `Image`.
- `SceneRenderer` exposes cull diagnostics (`GetLastVisibleCount` / `GetFrustumSurvivedCount` /
  `GetLastDrawnCount` / `GetLastGpuSurvivorCount` / `DidBroadphaseRebuildLastFrame` /
  `GetBroadphaseNodeCount`); `Context::GetLastGpuFrameTimeMs()` is the completed-frame GPU time.
- stb is already vendored for the engine (`engine/src/Vendor/StbImage.cpp`), but that is the engine's
  TU; `veng::mcp` needs its **own** access to `stb_image_write` for the PNG encode (it does not link
  `libveng`'s private vendor symbols).

## What lands

### 1. The `Viewport` provider is populated

Plan 01's `McpHost::Viewport` (`function<Renderer::Viewport*(string_view name)>`) becomes live: a
game fills it to return its primary viewport for a well-known name (e.g. `""` or `"primary"`) and
`nullptr` otherwise; the editor (Plan 04) returns a named panel's viewport. `Application` has no
public full-viewport-list accessor, so **the app supplies the mapping** — the server never enumerates
the engine drive-list itself. `render.list_viewports` (below) reports whatever the provider chooses
to expose, via a companion `function<vector<string>()> ViewportNames` added to `McpHost` (so the
provider both *names* and *resolves* its viewports). A host that sets neither leaves the render tools
returning "no viewports", not a crash.

### 2. PNG encode

stb_image_write is vendored into a dedicated `veng::mcp` TU (`mcp/src/Vendor/StbImageWrite.cpp`, its
own `.clang-tidy` `-*` override), so the lib stays self-contained and does not reach into
`libveng`'s private vendor symbols. A small `EncodePng(width, height, rgb8)` → `vector<u8>` wraps
`stbi_write_png_to_func`.

### 3. The render tools (`RegisterRenderTools(McpServer&, const McpHost&)`)

All run under `Pump()` on the render thread:

- **`render.screenshot`** — arg `{ viewport?: string }` (default the primary). Resolves the viewport
  through `McpHost::Viewport`, `Download()`s its RGBA16F output, tonemaps-to-8-bit the same way the
  smoke capture does (clamp `unpackHalf1x16` per channel → 8-bit RGB), PNG-encodes, base64s, and
  returns an **MCP image content block** (`{ content: [{ type: "image", data: <base64>,
  mimeType: "image/png" }] }`) plus the pixel dimensions. A missing viewport is an `isError` result.
  Because `Download()` blocks on a GPU copy and runs at the pump point on the render thread, it is
  serialized with the frame — no concurrent access to the output image.
- **`render.list_viewports`** — the names from `McpHost::ViewportNames`, each with its region extent
  and role (`Presented`/`Offscreen`) where resolvable.
- **`render.stats`** — arg `{ viewport?: string }` → that viewport's `SceneRenderer` cull funnel
  (`visible`/`frustum_survived`/`drawn`/`gpu_survivors`, `broadphase_rebuilt`, `broadphase_nodes`)
  plus `Context::GetLastGpuFrameTimeMs()`. The engine's existing counters, surfaced to an agent.

## Notes & constraints

- **The screenshot is the tonemapped scene-color output, 8-bit** — the same bytes the golden PPM
  captures, re-encoded as PNG. It is not a raw HDR dump; an agent wants a viewable image. (A raw/HDR
  or a specific-debug-view capture — driving `SceneRendererSettings::DebugView` — is a natural
  additive tool later; note it, do not build it.)
- **One `Download()` per call, on demand** — no persistent readback ring. The tool is for occasional
  agent inspection, not a video feed; streaming frames rides the future SSE work.
- The image content block's base64 can be large (a 1280×720 PNG). That is inherent to returning an
  image over JSON-RPC; the tool documents that a caller can pass a smaller viewport or (later) a
  downscale arg.

## Files (sketch)

- `mcp/include/Veng/Mcp/McpHost.h` — add `ViewportNames` beside `Viewport`.
- `mcp/src/Vendor/StbImageWrite.cpp`, `mcp/src/Vendor/.clang-tidy` — the PNG-encode TU.
- `mcp/src/RenderTools.cpp` (new) — `RegisterRenderTools` + the three handlers + `EncodePng` + base64.
- `mcp/src/McpServer.cpp` — call `RegisterRenderTools` on construction.

## Verification

- A **GPU** test (`mcp_screenshot`, labelled `gpu`, `SKIP_RETURN_CODE 77`, skips with no ICD like the
  rest of the GPU band): headless `Application` with a managed viewport rendering a known scene, wire
  the `McpHost::Viewport` to the primary viewport, drive the pump, call `render.screenshot` over
  loopback, decode the returned base64 PNG, and assert the dimensions match the viewport extent (and
  optionally a coarse non-blank check). `render.stats` returns plausible counts.
- The default-band `mcp_world`/`mcp_loopback` tests stay green (render tools that need no viewport
  degrade to "no viewports", not a failure).
- `ctest` (including the `gpu` band where a device exists) green; `include_hygiene` green.
