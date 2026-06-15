# Game-module build model — design overview

> **The build model is DELIVERED — [planset-9](../planset-9/README.md)** (stream A).
> A game is now `libgame` (shared, the runtime) + a thin launcher that `dlopen`s it
> through a single C-ABI `VengModuleRegister` entry; `veng_add_game` emits the pair
> in-tree, and `hello-triangle` ships as `libhello_triangle` + a launcher. This doc
> keeps the **enduring seams** the model fixed and the **editor-only forward work**
> that builds on them (the type-reflection layer, `libgame_editor`/`EditorRegistry`,
> and installed-package wiring). Direction for the editor area
> ([README](README.md), [editor.md](editor.md)) — not a firm plan; the remaining
> pieces become their own plansets when taken up.

## Why (the change that shipped)

A game used to **be** an executable: `examples/hello-triangle/main.cpp` subclassed
`Application`, defined `int main()`, and linked `libveng` into one binary. A type
defined in that executable — a component, a system, a custom asset type — was
private to it; nothing outside the process could name it.

The editor needs the opposite. To edit and preview a game's content it must **load
the game's native types**: enumerate component types, construct them, read/write
their fields, drive its custom asset loaders. And the *same* game logic must still
run **standalone**, with no editor present. One body of game code, two hosts that
load it — the **launcher** (the shipped product) and the **editor** (the authoring
environment) — so the game's code lives in a **shared library** and the executable
shrinks to a thin launcher. planset-9 delivered the launcher half of that.

## The three artifacts

A game splits into up to three CMake targets where there was one; **planset-9 builds
the first two**:

```
libgame         (shared)  the game's RUNTIME: Application logic, components,
                          systems, custom runtime asset types. Links libveng.
                          NO editor code.  ── shipped.        ── DONE (planset-9)

game-launcher   (exe)     thin wrapper: dlopens libgame, constructs the app via
                          its registered factory, runs the loop. The shipped
                          standalone product.  ── shipped.    ── DONE (planset-9)

libgame_editor  (shared)  the game's EDITOR extensions: custom panels, custom
                          inspectors, asset editors for the game's own types.
                          Links libveng + libveng_editor + libgame.
                          Loaded ONLY by the editor.  ── never shipped.  ── future.
```

`libgame_editor` cannot be built until `libveng_editor` exists (the editor planset).
The ABI is shaped to accept it now (the reserved-null `EditorRegistry*`, below)
without pulling it forward.

## Seam 1 — the module entry point (delivered)

A host that `dlopen`s a module needs **one** exported symbol to call; everything
else the module exposes, it exposes by **registering** through that call (C++ has no
reflection — the host cannot discover a module's types by inspection). A single
C-ABI entry, resolved by name after load:

```cpp
extern "C" VE_MODULE_EXPORT void VengModuleRegister(VengModuleHost* host);
```

`VengModuleHost` is the host's side of the contract — the registries the module
fills. planset-9 ships it as **inert registries, no live engine objects**:

```cpp
struct VengModuleHost
{
    ApplicationRegistry& App;     // the module registers its Application factory here
    EditorRegistry*      Editor;  // non-null ONLY when loaded by the editor; null otherwise
};
```

- `libgame` registers its **`Application` factory** through `host->App`; the launcher
  reads it back and `Run()`s it. It ignores `Editor` (null in the launcher).
- `libgame_editor` (future) will register **editor** things — panels, inspectors,
  asset editors — through `host->Editor` (guaranteed non-null; only the editor loads
  it).

The editor planset extends this struct **additively** with its reflection registry
(a `TypeRegistry&`) — a change to a boundary nothing ships against yet.

## Seam 2 — type reflection / descriptors (the editor-shell planset's first task)

> **Deferred out of the build-model prerequisite into the editor-shell sub-area**
> ([editor.md](editor.md)). It has no consumer until the inspector exists, so it is
> designed against that real client rather than speculatively here. The **resolved
> direction** is recorded so that planset does not rebuild it blind.

The editor's auto-generated inspectors ("select an entity, edit its fields") require
the editor to know a type's **fields** — names, types, offsets — at runtime. C++
gives none of that, so the game must **describe** its types through a hand-written
descriptor layer (no codegen v1; a `VE_REFLECT(...)` macro is sugar to consider
later, a clang-AST pass only if the hand-written burden proves real):

```cpp
struct FieldDescriptor
{
    string_view Name;
    TypeId      Type;        // an OPEN id, not a closed enum (see below)
    FieldClass  Class;       // a small closed meta-kind for generic handling
    usize       Offset;      // offsetof(T, member)
};

struct TypeDescriptor
{
    string_view             Name;
    usize                   Size;
    function<void*()>       Construct;
    vector<FieldDescriptor> Fields;
};
```

The **resolved decisions** for that layer:

- **Field types are an open `TypeId`, not a closed engine enum.** Engine builtins —
  including `AssetHandle<Texture/Mesh/Material>` — are pre-registered **identically**
  to a game's own asset types, so a game shipping a new asset type **extends the
  vocabulary with no engine change**.
- **A small closed `FieldClass` meta-kind** rides alongside the open `TypeId` to
  carry generic handling (e.g. "this is an asset-handle field", "this is a scalar").
- **Inheritance is single, non-virtual, base at offset 0**, walked base-first.

The editor walks `TypeDescriptor::Fields` to build an inspector with a widget per
field type. This `TypeRegistry` layer is **shared** between game-module registration
and the editor's inspector framework — designed once, in the editor-shell planset,
as the contract for both.

## Seam 3 — the ABI boundary (delivered)

A shared-library boundary is an ABI boundary. The rules planset-9 fixed:

- **One toolchain, one STL, one flag set on both sides.** Module and host are built
  by the same compiler with the same standard library and the same `-fno-exceptions`,
  recompiled from one tree. veng is **not a binary-plugin platform** — that is what
  makes passing `string` / `vector` / `Ref<T>` across the boundary safe (a mismatched
  STL across a DLL boundary is the classic heap-corruption trap).
- **The *entry point* is C ABI; the *payload* is C++.** `VengModuleRegister` is
  `extern "C"` so the symbol resolves robustly; a one-integer `VengModuleAbiVersion`
  handshake (checked by `ModuleLoader` before the entry runs) makes a stale module
  fail **loudly at load**, not mysteriously later. Once the entry is called, rich
  engine types flow freely.
- **Export visibility.** `VE_MODULE_EXPORT` carries unconditional export semantics on
  the module entry (it is never `dllimport` — `VE_API` cannot be reused for it).
  Visibility stays **default-visible**: planset-9 did **not** flip `libveng` to
  hidden visibility — that hardening bites only on the Windows port (where `VE_API`'s
  dllimport/dllexport is load-bearing) and is bundled there.
- **Ownership across the boundary.** The **host owns the registries**; a module
  *registers into* them and the host drops the entries on unload. A module never
  frees a host object and vice versa.

## What stays out — hot-reloading game code

Recompiling `libgame` and swapping it into a running editor without restarting the
play session is **notoriously hard** in C++ — dangling vtables, stale function
pointers, file-static state, live objects whose layout changed. **Out of scope.** A
game-code reload restarts the play session (unload module → reload → reconstruct), a
later stretch gated on a serialize-across-reload story.

> **Don't conflate it with *asset* hot-reload.** Live-previewing a material as you
> edit its graph is *asset* reload — recook + re-upload behind a stable
> `AssetHandle` — and comes from the [asset system's async path](asset-system.md)
> ([threading](threading-task-system.md)), **not** from reloading the game DLL.

## CMake surface

`veng_add_game(...)` emits the shared lib + the launcher from one declaration:

```cmake
veng_add_game(my_game
    SOURCES      src/Game.cpp src/Components.cpp ...
    ASSET_PACK   my_game_assets)                # an add_asset_pack target to copy beside the launcher
```

It produces `libmy_game` (shared) and `my_game-launcher` (exe), copies the cooked
pack beside the launcher, and sets the `$ORIGIN`/`@loader_path` rpath so the trio is
relocatable. The **`EDITOR` arm** (`libmy_game_editor`, excluded from the shipped
set) is **future** — `libveng_editor` does not exist yet.

`veng_add_game` ships **in-tree only** (it serves the example and the tests, which
build veng as the top-level project). **Installed-package wiring remains forward
work:** to make it callable from a downstream project consuming an *installed* veng
via `find_package(veng)`, install the helper `.cmake` files (`Game.cmake`,
`AssetPack.cmake`) + `launcher_main.cpp`, `include()` them from
`veng-config.cmake.in`, and resolve `VENG_LAUNCHER_MAIN` to the installed path. This
also fixes `AssetPack.cmake`'s pre-existing missing install. A full shipped product
additionally ships `libveng` beside the launcher (the in-tree launcher resolves
`libveng` via its build rpath, so the in-tree relocatable copy need not include it).

## Resolved decisions

- **Module entry shape — single `VengModuleRegister(host)`.** One uniform entry,
  resolved by name, for every module and host. (Not multiple named exports.)
- **Host carries inert registries — no live `Context`/`AssetManager`.** Registration
  is a *factory* (no GPU work), so the entry needs no live engine objects:
  `VengModuleHost` is `{ ApplicationRegistry& App; EditorRegistry* Editor; }`, and
  `Application` keeps owning `Context`/`AssetManager`/`TaskSystem` unchanged. This
  **departs from this doc's original sketch** (`Context&`/`AssetManager&` on the
  host): passing live objects in would force inverting `Application`'s ownership for
  no benefit and contradict "nothing new in the engine runtime." One consequence:
  registering **custom asset-type loaders** genuinely needs a live `AssetManager`, so
  it is **out of scope** until a custom asset type exists to drive it; it returns
  with the host's `AssetManager&` when one does.
- **Reflection deferred to the editor, with the open-`TypeId` direction** (above) —
  no consumer until the inspector exists.
- **A uniform, veng-provided launcher** + the `VengModuleAbiVersion` handshake +
  **executable-relative resolution** (the module beside the launcher via
  `$ORIGIN`/`@loader_path`, assets + cache via `ExecutableDirectory()`).
- **The smoke routes through the launcher + loader** — one shipping code path, no
  direct-link fast path. (The registered `headless_smoke` ctest links `veng::veng`
  directly and is untouched; the new `hello_triangle_launcher_smoke` exercises the
  loader path.)
- **Default-visible** — the hidden-visibility audit is deferred to the Windows port.
- **`EditorRegistry*` reserved and null** — forward-declared in `libveng`, always
  null in the launcher, non-null only for the future editor host.
- **Game-code hot-reload is out** — restart the play session.
