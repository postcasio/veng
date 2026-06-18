# Plan 02 — hello-triangle game-tier proof

**Goal:** migrate `examples/hello-triangle/main.cpp`'s debug UI fully off raw `ImGui::`
onto `Veng::UI`. This is the **proof that a game module authors UI with zero `ImGui::` at
widget sites** — the load-bearing validation of the engine-tier scope decision (a game does
not link `libveng_editor` for a debug panel).

**Depends on** plans 00 (widgets) and 01 (scopes).

## The call sites

The debug UI is two panels in `RenderUserInterface()`
(`examples/hello-triangle/main.cpp:246–272`):

| Today | `Veng::UI` |
|---|---|
| `ImGui::Begin("Scene"); … ImGui::End();` | `if (auto w = UI::Window("Scene")) { … }` |
| `ImGui::Combo("View", &mode, modeNames, IM_ARRAYSIZE(modeNames))` | `UI::Combo("View", mode, modeNames)` |
| `ImVec2 available = ImGui::GetContentRegionAvail();` | `vec2 available = UI::ContentRegionAvail();` |
| `ImGui::Image(static_cast<ImTextureID>(m_Composite->GetSceneTexture().GetTextureId()), …)` | `UI::Image(m_Composite->GetSceneTextureRef(), { available.x, available.y })` |
| `ImGui::Begin("Stats"); … ImGui::End();` | `if (auto w = UI::Window("Stats")) { … }` |
| `ImGui::Text("%.1f fps (%.2f ms)", ImGui::GetIO().Framerate, …)` | `UI::Text(fmt::format("{:.1f} fps ({:.2f} ms)", UI::FrameRate(), 1000.0f / UI::FrameRate()))` |

## Decisions

1. **`Combo` over a `string_view` array.** The current `const char* modeNames[]` +
   `IM_ARRAYSIZE` becomes a `std::array<string_view, N>` (or a `static constexpr
   string_view[]`) passed as the span `Combo` overload takes (plan 00). `mode` stays a
   plain `i32` selection index.

2. **`UI::Image` needs a `Ref<ImGuiTexture>`, not a raw texture id.** The current site
   passes `GetSceneTexture().GetTextureId()` (a `u64`). The `UI::Image` overload takes
   `const Ref<ImGuiTexture>&` (plan 00) so the engine owns the id→`ImTextureID` cast. This
   plan adds **`ImGuiCompositePass::GetSceneTextureRef() → const Ref<ImGuiTexture>&`**
   alongside the existing `GetSceneTexture() → ImGuiTexture&` (the composite pass already
   owns the texture as a `Ref` internally — it exposes the reference form for the
   `UI::Image` consumer). The `&`-returning accessor stays for any caller still needing the
   raw handle; the sample uses the new ref form.

3. **`UI::FrameRate()` replaces `ImGui::GetIO().Framerate`** (plan 00, planset decision 5).
   `GetIO()` is host plumbing that stays raw *in the integration layer* (`ImGuiLayer`), but
   a framerate readout in a panel is a widget-authoring stat — `FrameRate()` is the thin
   wrapper so the sample reaches zero raw `ImGui::`. Calling it twice (fps + ms) is fine;
   if preferred, bind it to a local `const f32 fps = UI::FrameRate();` first.

4. **Text via `fmt::format`** (planset decision 3) — the `"%.1f fps (%.2f ms)"` printf
   becomes a `fmt` format string, the one formatting idiom.

## The proof

After this plan, `grep "ImGui::" examples/hello-triangle/main.cpp` returns **only** the
comment lines (the `RenderUserInterface()` doc comment naming `ImGui::Image`), with **no
`ImGui::` call expression** remaining — and `examples/hello-triangle` links
`libhello_triangle` against `veng::veng` only (it already does; it never linked
`libveng_editor`). A game module authored its entire debug UI on `Veng::UI`.

The comment at `main.cpp:218` and `:246`-adjacent that names `ImGui::Image`/`ImGui` is
updated to name `UI::Image` / `Veng::UI` (present-tense fact; the comment policy applies).

## Files

| File | Change |
|---|---|
| `examples/hello-triangle/main.cpp` | Migrate the two debug panels to `Veng::UI`; `#include <Veng/UI/UI.h>`; drop the `modeNames` C-array for a `string_view` span; doc comments name `Veng::UI`. |
| `engine/include/Veng/Renderer/ImGuiCompositePass.h` | Add `GetSceneTextureRef() → const Ref<ImGuiTexture>&`. |
| `engine/src/Renderer/ImGuiCompositePass.cpp` | Implement the ref accessor (return the owned `Ref`). |

## Verification

- Clean build — `libhello_triangle` compiles against `Veng::UI`.
- **The zero-`ImGui::` grep** (above) — the acceptance check for this plan.
- `hello_triangle_launcher_smoke` (gpu band) exits 0; `smoke_golden` matches the golden —
  the smoke path renders headless with no ImGui panels, so the capture is byte-for-byte
  unaffected. The windowed app's debug panels render identically (same underlying ImGui
  calls).
- `include_hygiene` unaffected (the sample is not a public header; the new
  `ImGuiCompositePass` accessor returns an engine `Ref<ImGuiTexture>`, no backend leak).
- Manual: run the windowed launcher, confirm the Scene/Stats panels draw, the View combo
  switches `DebugView`, and the scene image fills the panel as before.
