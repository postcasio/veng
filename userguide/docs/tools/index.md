# Tools

veng ships two tools alongside the runtime:

- **[The vengc cooker](vengc.md)** — the command-line tool that turns hand-written
  JSON into the binary packs the engine loads: compiling shaders, validating
  materials, and checking prefabs against your types.
- **[The editor](editor.md)** — a reflection-driven editor framework for building
  tools on top of veng, with panels, an inspector, a node graph, and live editing.

The **[`Veng::UI`](../ui.md)** debug-UI wrapper that both your game and the editor
use is covered on its own page.
