# Game-module build model — design overview (future)

> **Vision / design sketch, not scheduled.** Detail for the **editor** area
> ([README](README.md), [editor.md](editor.md)) — specifically its prerequisite:
> the change that turns a game from a self-contained executable into a **shared
> library a host loads**. Direction, surfaces, and recommendations, not a firm
> plan; it becomes its own planset when taken up. The editor depends on this the
> way the [asset system](asset-system.md) depends on
> [threading](threading-task-system.md).

## Why

Today a game **is** an executable. `examples/hello-triangle/main.cpp` subclasses
`Application`, defines `int main()`, and links `libveng` straight into one binary.
A type defined in that executable — a component, a system, a custom asset type —
is private to it: nothing outside the process can name it.

The editor needs to do exactly that. To edit and preview a game's content it must
**load the game's native types**: enumerate its component types, construct them,
read/write their fields, drive its custom asset loaders. And the *same* game logic
must still run **standalone**, with no editor present. One body of game code, two
hosts that load it:

- the **standalone launcher** (the shipped product), and
- the **editor** (the authoring environment).

A type that lives in an `.exe` can be loaded by neither a second `.exe` nor the
editor. So the game's code moves into a **shared library** (`.dylib` / `.so` /
`.dll`), and the executable shrinks to a thin launcher that loads it. This is the
foundational build change the whole editor stands on.

## The three artifacts

A game splits into three CMake targets where there was one:

```
libgame         (shared)  the game's RUNTIME: Application logic, components,
                          systems, custom runtime asset types. Links libveng.
                          NO editor code. ── shipped.

game-launcher   (exe)     thin wrapper: loads libgame, constructs the app,
                          runs the loop. The shipped standalone product. ── shipped.

libgame_editor  (shared)  the game's EDITOR extensions: custom panels, custom
                          inspectors, asset editors for the game's own types.
                          Links libveng + libveng_editor + libgame.
                          Loaded ONLY by the editor. ── never shipped with the game.
```

The split is the point. **`libgame` carries no editor code** — the runtime product
pays nothing for the authoring tools. **`libgame_editor` carries the views and
tools** the editor draws ([editor.md](editor.md) — "custom editor views and tools,
not part of the game itself"), and only the editor ever loads it. The launcher and
the editor are both *hosts*: each `dlopen`s `libgame` (the editor also loads
`libgame_editor`), calls its entry point, and drives it.

```
        ┌────────────── game-launcher (exe) ──────────────┐
ship ►  │  loads libgame → constructs Application → Run()  │
        └─────────────────────────────────────────────────┘

        ┌──────────────────── editor (exe) ───────────────────────┐
dev  ►  │  loads libgame  (its types, runtime asset loaders)       │
        │  loads libgame_editor  (its panels, inspectors, editors) │
        └──────────────────────────────────────────────────────────┘
```

## Design axis 1 — the module entry point

A host that `dlopen`s a module needs **one** exported symbol to call. Everything
else the module exposes, it exposes by **registering** through that call — C++ has
no reflection, so the host cannot discover a module's types by inspection. The
module hands the host a table.

A single C-ABI entry point, resolved by name after load:

```cpp
// In libveng (or a small shared "module ABI" header both sides include):
extern "C"
{
    // Exported by every game / editor module. The host dlsym()s this name,
    // calls it once after load, and the module registers what it provides
    // into the registries the host passes in.
    VE_API void VengModuleRegister(VengModuleHost* host);
}
```

`VengModuleHost` is the **host's side** of the contract — the registries the
module fills:

```cpp
struct VengModuleHost
{
    Context&        RenderContext;     // the live engine context
    AssetManager&   Assets;            // for custom asset-type registration
    TypeRegistry&   Types;             // component / asset-type descriptors (reflection)
    EditorRegistry* Editor;            // non-null ONLY when loaded by the editor
};
```

- `libgame` registers **runtime** things: component types, systems, custom asset
  loaders. It uses `Types` and `Assets`; it ignores `Editor` (null in the
  launcher).
- `libgame_editor` registers **editor** things: panels, inspectors, asset editors.
  It uses `Editor` (guaranteed non-null — only the editor loads it).

The same entry-point name for both modules keeps the loader uniform; what a module
*does* in it depends on which registries it touches.

## Design axis 2 — reflection / type descriptors (the hard part)

The editor's auto-generated inspectors ("select an entity, edit its fields")
require the editor to know a type's **fields** — names, types, offsets — at
runtime. C++ gives it none of that. The game must **describe** its types.

A lightweight, hand-written **type-descriptor** layer (no codegen v1):

```cpp
struct FieldDescriptor
{
    string_view Name;
    FieldType   Type;        // F32, Vec3, Vec4, Bool, AssetHandleTexture, ...
    usize       Offset;      // offsetof(T, member)
};

struct TypeDescriptor
{
    string_view             Name;
    usize                   Size;
    function<void*()>       Construct;        // placement/factory
    vector<FieldDescriptor> Fields;
};

// The game registers each exposed type:
void VengModuleRegister(VengModuleHost* host)
{
    host->Types.Register<Transform>("Transform", {
        { "Position", FieldType::Vec3, offsetof(Transform, Position) },
        { "Rotation", FieldType::Quat, offsetof(Transform, Rotation) },
        { "Scale",    FieldType::Vec3, offsetof(Transform, Scale)    },
    });
}
```

The editor walks `TypeDescriptor::Fields` to build an inspector with a widget per
`FieldType` ([editor.md](editor.md) — reflection-driven inspectors). Recommendation:
**hand-written descriptors v1**, a `VE_REFLECT(...)` macro for sugar later, and a
codegen/clang-AST pass only if the hand-written burden proves real. Start with the
mechanism that needs no toolchain.

This `TypeRegistry` / descriptor layer is **shared** between the game-module work
and the editor's inspector framework — design it once, here, as the contract.

## Design axis 3 — the ABI boundary (get this right or it crashes)

A shared-library boundary is an ABI boundary. The rules veng must hold:

- **One toolchain, one STL, one flag set on both sides.** Module and host must be
  built by the same compiler with the same standard library and the same
  `-fno-exceptions`. veng is **not a binary-plugin platform** — modules are
  recompiled with the engine, from the same source tree / toolchain. State this
  assumption loudly; it is what makes passing `string` / `vector` / `Ref<T>`
  across the boundary safe (mismatched STLs across a DLL boundary is the classic
  way to corrupt the heap).
- **The *entry point* is C ABI; the *payload* is C++.** `VengModuleRegister` is
  `extern "C"` with POD/`VE_API` parameters so the symbol resolves robustly and a
  stale module fails loudly at load, not mysteriously later. Once it is called,
  rich engine types flow freely — justified by the same-build assumption above.
- **Export visibility.** Extend the existing `VE_API` macro (already present for
  the anticipated Windows port — `__declspec(dllexport/import)` / `visibility`)
  to cover the game/editor module surface. Default-hidden visibility, explicit
  exports.
- **Ownership across the boundary.** The **host owns the registries**; a module
  *registers into* them and the host drops the entries on unload. A module never
  frees a host object and vice versa. Engine objects (`Context`, `AssetManager`)
  are passed by reference and owned by the host for the module's whole lifetime.

## What stays out — hot-reloading game code

The tantalizing feature: recompile `libgame`, swap it into the running editor
without restarting the play session. In C++ this is **notoriously hard** —
dangling vtables, stale function pointers, file-static state, live objects whose
layout changed under them. **Out of scope v1.** v1 reloads game code by restarting
the editor's play session (unload module → reload → reconstruct). Note it as a
later stretch, gated on a serialize-across-reload story.

> **Don't conflate it with *asset* hot-reload.** Live-previewing a material as you
> edit its graph is *asset* reload — recook + re-upload behind a stable
> `AssetHandle` — and comes from the [asset system's async/hot-reload path](asset-system.md)
> ([threading](threading-task-system.md)), **not** from reloading the game DLL.
> The editor gets live content preview without ever reloading native code.

## CMake surface

A `veng_add_game(...)` function, mirroring planset-5's `add_asset_pack`, that emits
the shared lib + the launcher from one declaration:

```cmake
veng_add_game(my_game
    SOURCES      src/Game.cpp src/Components.cpp ...
    EDITOR       src/Editor/MaterialNodes.cpp ...   # optional: builds libmy_game_editor
    ASSET_PACK   assets/game.vengpack.json)         # reuses add_asset_pack
```

It produces `libmy_game` (shared), `my_game-launcher` (exe), and — when `EDITOR`
sources are given — `libmy_game_editor` (shared, excluded from the shipped set).

## Migrating the sample

`hello-triangle` is the natural acceptance test **and** the smoke test, so migrate
it carefully. It becomes `libhello_triangle` + a launcher; the headless smoke path
(`HT_SMOKE`) must keep working through the launcher (the launcher constructs the
app the same way, just after a `dlopen`). Verify the smoke binary still writes a
correct-sized PPM through the new two-artifact shape before calling the plan done.
Whether the minimal smoke keeps a direct-link path for simplicity vs. going fully
through the module loader is an open question (below).

## Touch points (what this phase adds / modifies)

- **New:** the module ABI (`VengModuleRegister` entry, `VengModuleHost`), a
  `ModuleLoader` (cross-platform `dlopen`/`LoadLibrary` wrapper in `libveng`), the
  `TypeRegistry` + `TypeDescriptor`/`FieldDescriptor` reflection layer.
- **`VE_API` / export macros:** extended to the module surface; default-hidden
  visibility audited.
- **`Application`:** must support being **constructed and driven by a host** (the
  launcher / editor) rather than always owning `main()` — a factory the module
  exports, or a registration that hands the host an `Application` to run.
- **CMake:** `veng_add_game(...)`; the example migrated to it.
- **Depends on:** nothing new in the engine runtime; this is a build/packaging +
  small-ABI change. The [editor](editor.md) is the consumer that needs it.

## Open decisions

- **Module entry shape** — a single `VengModuleRegister(host)` (recommended) vs.
  multiple named exports (`VengCreateApplication`, `VengRegisterEditor`, …). One
  uniform entry keeps the loader simple.
- **Reflection mechanism** — hand-written descriptors (recommend v1) vs. a
  `VE_REFLECT` macro vs. clang-AST codegen. Start hand-written; add sugar only if
  the burden is real.
- **Where the registries live** — `TypeRegistry` in `libveng` (so the runtime can
  use component descriptors for serialization too) vs. editor-only. Leaning engine,
  since a scene/entity model would want descriptors for save/load regardless.
- **Standalone launcher: thin or registering?** — does the launcher only construct
  the `Application`, or also let `libgame` register runtime systems (so the same
  registration path drives both hosts)? Leaning uniform — both hosts call
  `VengModuleRegister`; only `Editor` differs.
- **Smoke path** — keep a direct-link fast path for the headless smoke, or route
  it through the module loader like everything else (one code path, slightly more
  to keep green).
- **Hot-reload of game code** — v1 **no** (restart the play session); revisit with
  a serialize-across-reload design if iteration speed demands it.
