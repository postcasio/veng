# Debug UI: `Veng::UI`

`Veng::UI` is the engine's wrapper over Dear ImGui. You write UI against it rather
than calling `ImGui::` directly — in both game code and the editor — and it's all a
game needs to draw a debug panel, without pulling in the editor framework.

## Widgets

```cpp
if (UI::Button("Reset")) {
    ResetCamera();
}

UI::Drag("Speed", m_Speed, DragOptions{ .Speed = 0.1f, .Min = 0.0f });
UI::Text(fmt::format("frame: {} ({:.2f} ms)", m_Frame, m_FrameMs));
```

- **One `Drag`, overloaded for `f32`, `vec2`, `vec3`, `vec4`, and `i32`.** Its
  options are plain structs (`.Speed`, `.Min`, `.Max`, `.Format`), not ImGui flags.
- **Every editable widget returns `bool`** — whether the value changed this frame.
- **Text takes a preformatted string**, so you write
  `UI::Text(fmt::format(...))` rather than passing printf-style arguments.

## RAII scope guards

Every begin/end and push/pop pair is an RAII scope guard that closes on scope
exit, so the close survives every early-out:

```cpp
{
    UI::Window window("Stats");
    if (!window.IsOpen()) {
        return;
    }
    UI::Text("...");
}   // window closes here, always
```

The family covers `Window` / `Child` / `TreeNode` / `CollapsingHeader` / `Table` /
`Menu` / `Popup` / `Disabled` / `PushId` / `StyleColor` / `StyleVar` and more.
Flags are engine vocabulary enums (`WindowFlags`, `TreeFlags`, `StyleColorId`,
`StyleVarId`), not `ImGui*Flags`.

## Reaching ImGui directly

`Veng::UI` covers widget authoring, which is what game code touches. The ImGui
frame lifecycle and the docking and present setup stay inside the engine; you don't
manage those. It also doesn't try to *hide* ImGui — if you drop down to a raw ImGui
call, glm's `vec2` and `vec4` convert to and from ImGui's own vector types
automatically, so there's no glue to write at the boundary.
