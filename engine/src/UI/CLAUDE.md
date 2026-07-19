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
- **Every editable widget returns `[[nodiscard]] bool`** ("changed"), keeping immediate-mode
  semantics.
- **Text is preformatted `string_view`, not printf varargs** — a caller writes
  `UI::Text(fmt::format("{}: {}", a, b))`. fmt is veng's one formatting idiom.
- **RAII scope guards** (`UI::Window`/`Child`/`TreeNode`/`CollapsingHeader`/`Table`/`Menu`/
  `Popup`/`Disabled`/`PushId`/`StyleColor`/`StyleVar`/…) replace every begin/end and
  push/pop pair, closing on scope exit so the close survives every early-out. Flags are
  engine vocab enums (`WindowFlags`, `TreeFlags`, `StyleColorId`, `StyleVarId`), not
  `ImGui*Flags`. `EditorPanel::GetWindowFlags()` returns `UI::WindowFlags`.
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
