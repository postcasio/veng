# 02 — Window ownership & engine-driven teardown

**Goal:** fix the inverted window ownership and make `Application::Run` tear the
engine down completely and in the right order. Nothing leaks at exit.

**Dependencies:** none (01 recommended first to avoid churn). Plan 10 (headless)
builds directly on this.

## Current state

- `Context::Initialize(const ContextInfo&)`
  (`include/Veng/Renderer/Backend/Context.h:41`) *creates* the `Window`, returns
  `Unique<Window>` to `Application` (`src/Application.cpp:16`), and keeps a
  non-owning `Window* m_Window` (`Context.h:97`).
- `ContextInfo` (`Context.h:28`) duplicates `ApplicationInfo` almost field for
  field — including `WindowInfo` and font paths — because the context does the
  window's job.
- `Application::Run` (`src/Application.cpp:54-57`) ends with `WaitIdle()` +
  `OnDispose()`. `Context::DisposeResources()`, `Context::Dispose()`, and
  `Window::Dispose()` are never called: device, instance, surface, GLFW window
  and `glfwTerminate` all leak/skip at exit.

## Decision: Application owns the window

Of the two options in the recommendations doc, take **`Application` creates the
window and lends it to the context**. Rationale: it matches the existing
`Unique<Window> Application::m_Window`, and it is the shape headless mode (plan
10) needs — a context that *optionally* receives a window, rather than a context
that manufactures one.

## Changes

1. **`Window` construction stands alone.** `Window::Create(const WindowInfo&)`
   already exists (`Window.h:34`). Move the GLFW init + window creation that
   currently runs inside `Context::Initialize` into `Window` proper, so a
   `Window` is fully constructed before any context exists. Surface creation
   (`Window::CreateSurface(const Context&)`, `Window.h:43`) stays
   context-dependent and is called by the context during init.
2. **`Context::Initialize(const ContextInfo&, Window* window)`** — context
   receives a borrowed window (nullable later, for plan 10). Remove
   `WindowInfo`, `DefaultFontPath`, `IconFontPath` from `ContextInfo`
   (fonts move with ImGui in plan 09; until then they can stay as plain
   `ContextInfo` fields, just no longer duplicated through window creation).
3. **`Application::Initialize`** (`src/Application.cpp:14`) becomes:
   create window from `m_Info.WindowInfo` → `m_RenderContext.Initialize(info,
   m_Window.get())` → `OnInitialize()`.
4. **Engine-driven teardown.** `Application::Run` after the main loop:
   ```
   WaitIdle()
   OnDispose()                      // consumer releases its resources first
   m_RenderContext.DisposeResources()  // sync frames, ImGui, swapchain, pools
   m_RenderContext.Dispose()           // device, allocator, instance, surface
   m_Window->Dispose()                 // GLFW window + glfwTerminate
   ```
   Audit `DisposeResources`/`Dispose` in `src/Renderer/Backend/Context.cpp` for
   the exact order: sync frames → ImGui resources → swapchain → pools → device
   → debug messenger → surface → instance, then window. Fix whatever the audit
   turns up (these paths have likely never run).
5. **`Window::~Window` / `Close` / `Dispose` cleanup** — `~Window` calls
   `Close()` (`Window.h:40`) but `Dispose() const` (`Window.h:53`) is a
   separate, never-called method. Collapse to one obvious lifecycle:
   destruction disposes; `Close()` just requests loop exit.

## Acceptance

- Running an app and closing the window exits with no validation-layer
  complaints about undestroyed device/instance/surface, and GLFW terminates.
- `ContextInfo` no longer contains `WindowInfo`.
- Window can be constructed before the context exists (verified by the new
  `Application::Initialize` order).
