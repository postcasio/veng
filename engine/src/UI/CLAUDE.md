# Veng::UI — the immediate-mode UI vocabulary

`Veng::UI` (`engine/include/Veng/UI/`, in `libveng`) is the engine-tier immediate-mode
vocabulary fronting ImGui, authored at every widget site by game modules and the editor alike.
Project-wide conventions live in [the root CLAUDE.md](../../../CLAUDE.md), the runtime overview
in [engine/CLAUDE.md](../../CLAUDE.md), the retained game-UI sibling `Veng::Gui` in
[../Gui/CLAUDE.md](../Gui/CLAUDE.md), and the editor framework in
[editor/CLAUDE.md](../../../editor/CLAUDE.md).

UI is authored against `Veng::UI`, not raw `ImGui::`, at every widget site — game modules and
the editor both. (`Veng::UI` is the only reason a game links a UI surface; it does not link the
editor framework for a debug slider.)

- **One `Drag`, overloaded on the value type** — `f32`/`vec2`/`vec3`/`vec4`/`i32`. Options
  are designated-initializer structs (`DragOptions`, `SliderOptions` — `.Speed`/`.Min`/
  `.Max`/`.Format`), never an `ImGui*Flags` value. Configurability is deliberately reduced
  for call-site consistency: only the knobs the engine wants UI authors to vary are exposed.
  The closed `FieldClass` set maps onto this closed overload set, so the reflection
  inspector's `Vector` dispatch is a single `Drag` call.
- **The reflection inspector is an engine surface** (`Veng/UI/Inspector.h`): `DrawFields` /
  `DrawFieldWidget` walk a reflected struct's `FieldDescriptor`s and draw a two-column
  `PropertyTable` row per field — a built-in widget per `FieldClass`
  (Scalar/Vector/Quaternion/String/Enum/Matrix/Struct/Variant/Array), with Category grouping and
  VisibleIf/EnabledIf gating (`Veng/Reflection/FieldGate.h`). Field classes the engine can't
  resolve on its own — `AssetHandle`, `Reference`, and per-`TypeId` custom widgets — route to
  consumer-supplied `InspectorHooks`; an unset hook draws a read-only fallback. So a **game UI**
  gets reflection-driven property editing linking only `libveng`, and the **editor** builds its
  asset-chip / entity-drop / registry inspector on the same walk (see
  [editor/CLAUDE.md](../../../editor/CLAUDE.md)) rather than duplicating it. Caller opens the
  `PropertyTable`; the inspector fills the rows. **`DrawFieldValue` is the same widget without the
  row** — no label, no row advance — for a caller laying out its own grid, where the column header
  already names the value; the editor's data-table cells are what it exists for. Struct, Variant,
  and Array draw nothing there and return false: they expand into several rows and have no
  single-value rendering, so a caller wanting them opens a property table and uses
  `DrawFieldWidget`.
- **The à-la-carte debug panels are free functions over a reference** (`Veng/UI/DebugPanels.h`:
  `RendererStatsPanel`, `PerformancePanel`, `RenderSettingsEditor`; `Veng/UI/VideoCapture.h`:
  `VideoCapturePanel`). None opens a window of its own or keeps static state — a caller wraps the
  panel it wants in its own `UI::Window` and holds any persistent editing state itself — so a game's
  debug shell and the editor place the same panel in their own menus and docking.
- **Every editable widget returns `[[nodiscard]] bool`** ("changed"), keeping immediate-mode
  semantics.
- **Text is preformatted `string_view`, not printf varargs** — a caller writes
  `UI::Text(fmt::format("{}: {}", a, b))`. fmt is veng's one formatting idiom.
- **RAII scope guards** (`UI::Window`/`Child`/`TreeNode`/`CollapsingHeader`/`Table`/`Menu`/
  `Popup`/`Disabled`/`PushId`/`StyleColor`/`StyleVar`/…) replace every begin/end and
  push/pop pair, closing on scope exit so the close survives every early-out. Flags are
  engine vocab enums (`WindowFlags`, `TreeFlags`, `StyleColorId`, `StyleVarId`), not
  `ImGui*Flags`. `EditorPanel::GetWindowFlags()` returns `UI::WindowFlags`.
- **`Popup` and `Modal` are the two dialog shapes**, both returning `ScopedPopup` and both raised
  by `OpenPopup(id)`. A `Popup` is a menu or picker: a click outside or Escape dismisses it, which
  is what a stray click *should* do to a candidate list. A `Modal` is a question that must be
  answered — it blocks everything behind it and neither click-away nor Escape closes it, so the
  only exits are the ones its body draws with `CloseCurrentPopup` (or the title-bar close button
  the optional `open` pointer adds). Pick by whether a dismissal has a safe meaning: the asset
  editors' unsaved-changes prompt is the tree's one modal, because "clicked elsewhere" is not an
  answer to Save / Discard / Cancel.
- **The `Veng/UI/` headers are imgui-free in their signatures and members.** `<imgui.h>`
  appears only under `engine/src/UI/` (the scope-guard dtors are defined out-of-line there).
  The one ImGui-adjacent type a signature names is the engine's own `ImGuiTexture`
  (`UI::Image(const Ref<ImGuiTexture>&, vec2)`), already an engine wrapper. This keeps the
  surface within `include_hygiene`'s existing guarantee.
- **ImGui stays a PUBLIC dependency** (wrapper-only) — the aim is call-site consistency and a
  tight surface, not hiding ImGui.
- **`ImVec2`/`ImVec4` convert implicitly to/from glm's `vec2`/`vec4`.** `Veng/Vendor/ImGuiConfig.h`
  injects the conversions through ImGui's `IM_VEC2_CLASS_EXTRA`/`IM_VEC4_CLASS_EXTRA` hooks, wired
  as the `IMGUI_USER_CONFIG` compile definition (PUBLIC on `veng`, so imgui's own aggregation TU
  and every consumer compile one identical `ImVec2`/`ImVec4` — the macros add member functions
  only, no data members, so layout is unchanged). So a raw imgui/imnodes call takes a `vec2`
  directly and its return reads straight into a `vec2`; no `ImVec2(v.x, v.y)` glue at the
  boundary. ImGui-native literals (theme colors/padding in `ImGuiLayer`) stay authored as
  `ImVec2`/`ImVec4` — the conversion is for the glm seam, not a reason to rewrite native values.
- **The boundary.** `Veng::UI` replaces `ImGui::` only at widget-authoring sites. ImGui's
  frame lifecycle (`CreateContext`/`NewFrame`/`Render`/`GetDrawData`/`GetIO`/…) and the
  host/dock/present plumbing (`DockSpaceOverViewport`/`UpdatePlatformWindows`/…) are the
  integration layer and stay raw in `ImGuiLayer`/`EditorHost`. Immediate-mode input queries
  for UI logic (double-click, right-click menu, delete-key shortcut) are wrapped in
  `Veng/UI/Query.h` over closed `UI::MouseButton`/`UI::Key` enums — distinct from the
  event/input system, which feeds gameplay; the `Key` enum is populated to the keys the call
  sites use. The only ImGui types remaining in panel src are those crossing into imnodes
  (`ImVec2` for node positioning), which is itself an editor-private dependency.

## The ImGui renderer

`ImGuiLayer` draws ImGui's lists itself, on engine primitives, so it carries no backend code and
every GPU backend gets it unchanged. Only the platform half (`imgui_impl_glfw`, input and window
queries) is a vendored ImGui backend.

- **The pass.** `Render` records into the frame's command buffer: ImGui's texture requests first,
  then — when the draw data is non-empty — one rendering scope over the layer's RGBA16F output,
  prepared with `PrepareForAccess` for the colour write and left `SampleGraphics` for the
  compositor. The core-pack `imgui.vert`/`imgui.frag` stages run through one graphics pipeline per
  target format (`ImDrawVert`'s layout, its RGBA8 colour normalized by the input assembler; the
  straight-alpha blend `BlendState::AlphaBlend`). Every list is packed into one vertex and one
  16-bit index buffer per frame in flight, host-mapped and grown when a frame outgrows it; a draw
  addresses its list by `VtxOffset`/`IdxOffset`, its clip rectangle becomes a scissor in
  framebuffer pixels, and the push constants carry the display-to-clip scale and translate with
  the draw's texture slots. ImGui's reset-render-state callback re-applies the pass's state; any
  other user callback is called.
- **An `ImTextureID` is two bindless slots.** The low 32 bits are a set-0 texture slot, the bits
  above the sampler slot, and the top bit is set so no id is zero (ImGui's invalid id)
  (`src/ImGui/ImGuiTextureNative.h`). `CreateTexture` registers its own view of the caller's image
  (so the registration keeps the image alive) and takes the registry-shared sampler matching the
  caller's sampler description, which the draw then samples through; destroying the
  `ImGuiTexture` releases the texture slot through the registry's deferred release.
- **ImGui's own textures** — the dynamic font atlas, and any `ImTextureData` registered with
  ImGui — are served on request (`ImGuiBackendFlags_RendererHasTextures`). A create makes an
  RGBA8 `Image` and registers it, sampled linearly and clamped; a create or update uploads the
  whole texture or the request's `UpdateRect` through a staging buffer copied in the frame's
  command buffer between `PrepareForAccess` transitions; a destroy releases the slot and drops
  the image, both retired past the frames in flight. Requests are serviced on a frame that draws
  nothing too.
- **The layer loads its shaders at construction** (`ImGuiLayer::Create` takes the
  `AssetManager`) and keeps their modules rather than the asset handles, so the manager need not
  outlive it.
