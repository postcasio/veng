# 10 — Headless context

**Goal:** a `Context` that initializes without window, surface, or swapchain —
off-screen render target only — enabling CI tests of renderer code and batch
tools (e.g. asset baking).

**Dependencies:** 02 (context no longer creates the window) and 09 (ImGui no
longer mandatory) — both prerequisites are exactly the couplings that make
headless impossible today. The Context singleton (`Context.h:75,94`) stays for
now; headless doesn't require removing it, just not requiring a window.

## Current state (post-02/09 assumptions)

After plan 02 the context *borrows* a `Window*`; after plan 09 it owns no ImGui.
Remaining window/surface dependencies inside `Context::Initialize`
(`src/Renderer/Backend/Context.cpp`): surface creation, present-queue family
selection (`QueueFamilyIndices::PresentFamily`, `Context.h:17-26`), swapchain
creation + `SwapChainSupport` querying, present-mode selection, and the
frame loop (`AcquireNextFrame`/`PresentFrame` assume a swapchain image).
`GetRequiredExtensions` queries GLFW for instance extensions.

## Design

- `Context::Initialize(const ContextInfo&, Window* window)` accepts
  `window == nullptr` ⇒ headless:
  - skip surface + swapchain; `PresentFamily` not required for
    `IsComplete()` — split into `IsComplete()` (graphics) and
    `CanPresent()`;
  - skip `VK_KHR_SWAPCHAIN_EXTENSION_NAME` and GLFW instance extensions
    (guard `glfwGetRequiredInstanceExtensions` — also skip `glfwInit`
    entirely in this mode);
  - `GetSwapChain()` asserts in headless mode (fatal, per plan 03) — or
    better, returns `SwapChain*` and callers inside veng null-check; pick
    whichever leaves fewer branches.
- **Frame loop without present:** `AcquireNextFrame` waits the frame fence and
  drains retire bins (plan 04) but skips image acquisition; `SubmitFrame`
  submits without present-wait/signal semaphores; `PresentFrame` asserts
  (nothing to present). Synchronization frames keep existing so the in-flight
  resource model is identical in both modes.
- **Off-screen target is the consumer's job** (they create `Image` +
  `ImageView` with `ColorAttachment | TransferSrc` and `Download()` results) —
  veng only needs to not *require* a swapchain. A convenience
  `Context::CreateOffscreenTarget(uvec2, Format)` can come later.
- **`Application` headless mode:** `ApplicationInfo` gains
  `bool Headless = false` (or `optional<WindowInfo>`); when set,
  `Application::Initialize` creates no window and `Run`'s loop condition
  switches from `m_Window->IsOpen()` to a consumer-controlled
  `RequestExit()` flag. (CI tests may bypass `Application` entirely and
  drive `Context` directly — both should work.)
- **CI smoke test** (the point of the feature): a `tests/headless_smoke.cpp`
  that initializes headless, clears an off-screen image via a render pass,
  downloads it, and asserts pixel values. Requires a Vulkan implementation on
  CI — lavapipe/SwiftShader on Linux runners; on macOS, MoltenVK is present on
  dev machines anyway. Wire as a CTest target; skip gracefully (not fail) when
  no Vulkan ICD is found.

## Acceptance

- `Context::Initialize(info, nullptr)` brings up instance/device/allocator/
  pools/sync frames with validation layers clean, no GLFW calls made.
- The headless smoke test renders, downloads, and verifies pixels in CI.
- Windowed path is byte-for-byte behaviorally unchanged (run the sample app).
