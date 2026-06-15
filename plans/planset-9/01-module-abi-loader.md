# Plan 01 — Module ABI + `ModuleLoader`

> **Stream A (game-module build model), plan 1 of 2.** The planset's headline stream;
> independent of streams B (03–04) and C (05–06). See the README's *Dependencies &
> dispatching* section.

**Goal:** define the shared module-ABI contract — `VengModuleHost`, the
`extern "C" VengModuleRegister(VengModuleHost*)` entry, the module-entry export macro,
an **ABI version handshake**, and the `ApplicationRegistry` app-factory slot the host
references (it must be a complete type for the host struct and the test) — and add
`ModuleLoader`, the cross-platform wrapper that loads a shared library, verifies the
module's ABI version, resolves the entry symbol, and reports a load failure as a
`Result`. Prove the whole load → version-check → call → register path with a tiny **test
module** and an integration test, **without touching the sample** — the example stays a
plain exe until plan 02 (which adds the launcher + `veng_add_game` that *consume*
`ApplicationRegistry`).

## Why this is its own plan

The ABI contract and the loader are the load-bearing mechanism; reshaping the example
into the two-artifact model (plan 02) is a mechanical consequence once a host can load
a module and receive its registration. Building and verifying the loader against a
purpose-built test module isolates the ABI surface — symbol export, robust resolution,
load-failure reporting, and a registration actually landing — from the build-system
churn of migrating the sample. It also gives the planset a permanent, driver-free
regression test for the boundary.

## The module-ABI header — `engine/include/Veng/Module/Module.h`

The host's side of the contract — the registries a module fills. Engine C++ types flow
across the boundary (justified by the same-toolchain assumption); only the **entry
symbol** is C ABI. `Module.h` `#include`s `ApplicationRegistry.h` (defined in this plan,
below) because the host has a **reference** member to it; `EditorRegistry` is only ever
held by **pointer**, so it stays an incomplete forward declaration.

```cpp
// Forward-declared, defined only in the editor framework (a later planset). libveng
// never sees its definition; a non-editor host passes Editor = nullptr. Held by
// pointer, so the incomplete type suffices.
class EditorRegistry;

// The host's side of the module contract: what a loaded module registers into. The
// host owns these for the module's whole lifetime; a module registers into them and
// never frees a host object. Registration is a factory — no live Context/AssetManager
// is needed here, so none is passed (Application keeps owning the engine objects).
struct VengModuleHost
{
    ApplicationRegistry& App;     // the module hands the host its Application factory
    EditorRegistry*      Editor;  // non-null ONLY when loaded by the editor; null otherwise
};
```

The editor planset extends this struct with its reflection registry (a `TypeRegistry&`)
— an additive change to a boundary nothing ships against yet. (Stated here as a fact
about the design, **not** as a code comment — a "later we will" comment is forbidden by
the house rules; the struct ships with the two members above and no forward-looking
comment.)

The dynamic Vulkan dispatcher's storage
(`VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE`) lives in `libveng`'s single
`Context.cpp` TU. A game module links `libveng` and **never includes the backend
`Vulkan.h`** (the `include_hygiene` test enforces that no public header pulls it in), so
a module compiles no vulkan.hpp TU and defines the storage nowhere — there is exactly
one dispatcher instance, in the one `libveng` image. (If a future editor module ever
links Vulkan directly, this becomes load-bearing; it does not here.)

The C-ABI entry point itself:

```cpp
extern "C"
{
    // Exported by every game / editor module. The host dlsym()s this exact name,
    // calls it once after load, and the module registers what it provides into the
    // registries the host passes in. C ABI so the symbol resolves robustly and a
    // stale module fails loudly at load; the VengModuleHost payload is rich C++.
    VE_MODULE_EXPORT void VengModuleRegister(VengModuleHost* host);
}
```

`VE_MODULE_EXPORT` is the entry-export macro:

```cpp
// In Veng.h, beside VE_API: the export attribute for a module's C-ABI entry point.
// A module always DEFINES and exports this symbol, so it must carry export semantics on
// every platform — never dllimport. VE_API cannot be reused: in a consumer VE_API is
// dllimport on Windows, which would mark the entry the module itself defines as an
// import (a link error). So VE_MODULE_EXPORT is unconditionally export.
#if defined(_WIN32)
#define VE_MODULE_EXPORT __declspec(dllexport)
#else
#define VE_MODULE_EXPORT __attribute__((visibility("default")))
#endif
```

### ABI version handshake

The same-toolchain rule (decision 9) makes the C++ payload safe **only** when host and
module were built from the same engine. A stale module — built against an older
`VengModuleHost` layout or entry contract — must fail **loudly at load**, not corrupt
state later. A one-integer handshake gives that:

```cpp
// Bumped whenever VengModuleHost's layout or the entry contract changes. Host and
// module each bake in the value from the header they compiled against; the loader
// compares them before calling VengModuleRegister. Guarded with #ifndef so a target
// can force a wrong value with a -D define (the bad_version_module test relies on this).
#ifndef VENG_MODULE_ABI_VERSION
#define VENG_MODULE_ABI_VERSION 1u
#endif

// A module drops this in exactly one translation unit to export the version it was
// built against, beside its VengModuleRegister. The loader resolves and checks it.
#define VE_EXPORT_MODULE_ABI() \
    extern "C" VE_MODULE_EXPORT Veng::u32 VengModuleAbiVersion() { return VENG_MODULE_ABI_VERSION; }
```

`Module.h` includes `ApplicationRegistry.h` and forward-declares `EditorRegistry`; it
pulls in no backend include.

## `ApplicationRegistry` — `engine/include/Veng/Module/ApplicationRegistry.h`

The host's slot for a module's `Application` factory — **defined here** (not deferred to
plan 02) because `VengModuleHost` holds it by reference and plan 01's `loader_test`
instantiates a `VengModuleHost`, both of which need the complete type. Plan 02 only
*consumes* it (the launcher constructs the app from it).

```cpp
// A module registers its Application factory in VengModuleRegister; the host (launcher /
// editor) reads it back to construct and Run() the app. One app per module —
// RegisterApplication asserts (fatal) if called twice. The caller passes the factory
// explicitly: it captures exactly the ApplicationInfo (and anything else) it wants, with
// the capture spelling it chooses. Application is forward-declared; the factory body that
// constructs the concrete app is written in the module TU, which includes Application.h.
class VE_API ApplicationRegistry
{
public:
    void RegisterApplication(function<Unique<Application>()> factory);

    [[nodiscard]] bool HasApplication() const;          // a factory was registered
    [[nodiscard]] Unique<Application> Create() const;   // construct it, or nullptr

private:
    function<Unique<Application>()> m_Factory;
};
```

`RegisterApplication`, `HasApplication`, and `Create` are non-templates defined in
`engine/src/Module/ApplicationRegistry.cpp` (the class is no longer header-only).
`RegisterApplication` does `VE_ASSERT(!m_Factory, "...")` to enforce one app per module —
the "asserts if called twice" contract lives in the impl, not the header.

Taking a `function<Unique<Application>()>` rather than a perfect-forwarding
`RegisterApplication<T>(Args&&...)` is deliberate: a forwarding factory captures the
constructor args into the stored closure, silently copying every `ApplicationInfo` member
(including `WindowInfo::EventCallback`, a `function<>`) and failing to compile for any
move-only member — a hidden constraint behind a signature that *looks* like it forwards.
The explicit-factory slot has no such trap; the module writes the lambda and owns its
captures.

`HasApplication()` lets the test confirm the slot was filled **without constructing** an
`Application` (so `loader_test` stays driver-free — no Context/Window is built).

## `ModuleLoader` — `engine/include/Veng/Module/ModuleLoader.h` + `engine/src/Module/ModuleLoader.cpp`

A thin RAII wrapper over the platform loader, in `libveng`:

```cpp
class VE_API LoadedModule
{
public:
    ~LoadedModule();                       // dlclose / FreeLibrary
    LoadedModule(LoadedModule&&) noexcept;
    LoadedModule& operator=(LoadedModule&&) noexcept;

    // Resolve the module's entry and call it once with the host. Asserts (fatal) if
    // the entry is missing — a module without VengModuleRegister is a build error
    // surfaced at load, not a recoverable condition.
    void Register(VengModuleHost& host) const;

private:
    friend class ModuleLoader;
    void* m_Handle = nullptr;              // dlopen handle (opaque; defined in .cpp)
};
```

The move ctor and move-assign **must null the source handle** (`other.m_Handle = nullptr`)
so the moved-from object's destructor does not `dlclose` a handle the destination now
owns; move-assign must also close any handle the destination already held before
overwriting it. `LoadedModule` is returned by value through `Result<LoadedModule>`, so the
moves run on the normal path — a missed null-out is a double-`dlclose`, fatal under
`-fno-exceptions`.

```cpp
class VE_API ModuleLoader
{
public:
    // Load a shared library by path and verify its ABI version. Recoverable: a
    // missing/unloadable file, a missing version symbol, or a version mismatch is a
    // Result error (the launcher reports it and exits), not an assert.
    [[nodiscard]] static Result<LoadedModule> Load(const path& modulePath);
};
```

Impl: `#if defined(_WIN32)` → `LoadLibraryW`/`GetProcAddress`/`FreeLibrary`; else
`dlopen(RTLD_NOW | RTLD_LOCAL)`/`dlsym`/`dlclose`. (The `#if` split exists because the
native string differs — `path::c_str()` is `const char*` for `dlopen`, `const wchar_t*`
for `LoadLibraryW` — not just the API names.) `Load` opens the library, then runs the
**version handshake**: resolve `"VengModuleAbiVersion"` — it is a **function**, so cast
the resolved symbol to `Veng::u32(*)()` and **call it**, not dereference it as data — and
if the symbol is missing (a foreign/too-old module) or the returned value
`!= VENG_MODULE_ABI_VERSION`, return
`std::unexpected("module built against ABI vN, engine expects vM")` and let the handle
close. A `dlopen`/`LoadLibrary` failure likewise returns
`std::unexpected(dlerror()/FormatMessage)`. Only a version-matched module yields a
`LoadedModule`. `Register` then resolves `"VengModuleRegister"`, `VE_ASSERT`s it is
non-null (a version-matched module **must** carry the entry — its absence is a build
error, not a recoverable condition), casts to `void(*)(VengModuleHost*)`, and calls it.
`RTLD_LOCAL` keeps a module's **own** exported symbols out of the global namespace, so
two modules loaded into one editor cannot interpose on each other's symbols. (It does
**not** give a module a private `libveng` — shared engine state lives in the single
`libveng` image both link; that is fine here, one game, one launcher.)

## Test module + integration test — `tests/module/`

- **`tests/module/test_module.cpp`** — a minimal SHARED library (`add_library(...
  SHARED)` registered in the **top-level `CMakeLists.txt`** inside the `VENG_BUILD_TESTS`
  block — where every test target lives; there is no `tests/CMakeLists.txt` — linking
  `veng::veng`) that invokes `VE_EXPORT_MODULE_ABI()` and exports `VengModuleRegister`;
  inside the entry it registers a trivial `Application` subclass via
  `host.App.RegisterApplication([]{ return Veng::Unique<Veng::Application>(new ProbeApp()); })`
  (the factory is *stored*, never invoked — so no Context/Window is constructed) and
  asserts `host.Editor` is null.
- **`tests/module/bad_version_module.cpp`** — the same minimal module compiled with a
  forced wrong version (`-DVENG_MODULE_ABI_VERSION=999999u` on that target only), so its
  exported `VengModuleAbiVersion` disagrees with the engine's. Proves the handshake
  rejects a stale module without ever calling its entry.
- **`tests/module/loader_test.cpp`** — builds a `VengModuleHost` over a real
  `ApplicationRegistry`, `ModuleLoader::Load`s the good test module by the path CMake
  bakes in (`$<TARGET_FILE:...>` → a compile definition), calls `Register`, and asserts:
  `App.HasApplication()` is true (the slot was filled, without constructing the app),
  `Editor` was observed null; that `Load` of the **wrong-version** module returns a
  `Result` **error** (and its entry is never reached); and that `Load` of a nonexistent
  path returns a `Result` error. Registered as a plain `add_test` (no GPU label —
  driver-free; no ICD needed).

This is the permanent boundary regression test: it loads a real `.dylib`/`.so`/`.dll`
through the real loader and checks a real registration landed.

## Acceptance

- Clean build (the test module builds as a shared lib); `ctest` green including the new
  `loader_test`; the three new public headers — `Module/Module.h`,
  `Module/ModuleLoader.h`, `Module/ApplicationRegistry.h` — are **added by hand** to the
  `#include` manifest in `tests/include_hygiene.cpp` (it is not auto-discovered) and the
  `include_hygiene` test builds green.
- `loader_test` proves load → version-check → `VengModuleRegister` → registration
  round-trips; that a **version mismatch** is a recoverable `Result` error whose entry
  is never called; and that a bad path is a recoverable `Result` error.
- `examples/hello-triangle` is **unchanged** — still a plain exe, still renders /
  smokes correctly. The validation gate is unaffected (no GPU change this plan).
