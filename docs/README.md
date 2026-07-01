# veng guides

Hand-written, task-oriented documentation for building with veng. These guides
teach the *patterns* — how the pieces fit and why they are shaped the way they
are. They are separate from the generated API reference, which documents every
symbol: build the Doxygen reference with `cmake --build build --target docs`
(output under `build/docs/html`) and read it alongside these guides.

## Guides

- **[Writing gameplay systems](guides/writing-gameplay-systems.md)** — the full
  path from an empty `SceneSystem` to a system running in a level: the system
  lifecycle, choosing the Sim or View phase, the Input → Intent → Movement
  pattern, configuring a system through components, registering it into the
  catalog, and a worked example built from scratch.
- **[Wiring a level](guides/wiring-a-level.md)** — the `Level` asset from the
  author's side: world prefab versus level-scoped data (game mode, the active
  system set, render settings), why a level is not a prefab, and the
  load-to-play flow.
- **[Consuming veng](guides/consuming-veng.md)** — discovering veng from a game
  project with `find_package(veng)`: the three consumption modes (in-tree, build
  tree, install prefix), the `veng_ROOT` / `CMAKE_PREFIX_PATH` /
  `FETCHCONTENT_SOURCE_DIR_VENG` discovery incantations, and the co-development
  loop that needs no reinstall.

Every type, macro, and method these guides name exists in the engine as written,
and the worked examples cross-reference the real
[hello-triangle](../examples/hello-triangle/) game module so the prose stays
honest against the code.
