# Plan 01 — The `TypeRegistry&` module seam + GPU-free registration

**Goal:** realize the additive **`TypeRegistry&`-on-`VengModuleHost`** seam
planset-9 reserved and planset-10 named. Component-type registration moves from
"the game calls `Register<T>` at startup" to **"the module registers its types in
`VengModuleRegister`"**, so the *same* call populates the registry in the launcher
and (plan 02) in the cooker. This forces the registry **out of `Application` and
host-side**, and pins a **GPU-free registration contract**. Pure CPU; module +
unit tests, including a headless no-device reflection test. The sample's *module
registration* moves; its render path does not.

## Why this is its own plan, and on the main thread

This is the contract-setting plan: the ABI boundary changes (a host field + a
version bump), the ownership of the `TypeRegistry` changes (superseding planset-10
decision 4), and the GPU-free property of registration becomes a tested guarantee.
Every later plan — the cooker load path, the prefab importer, the runtime loader
— depends on this one uniform registration path existing. Isolating it keeps the
ABI + ownership decision a single reviewable surface.

## The ABI seam — `engine/include/Veng/Module/Module.h`

Add `TypeRegistry& Types` to the host and bump the ABI version:

```cpp
namespace Veng
{
    class EditorRegistry; // still forward-declared; null in non-editor hosts

    struct VengModuleHost
    {
        ApplicationRegistry& App;     // the module's Application factory
        TypeRegistry&        Types;   // the module registers its component/type descriptors
        EditorRegistry*      Editor;  // non-null ONLY under the editor host
    };
}

// host layout changed → a module built against ABI 1 must fail loudly at load.
#ifndef VENG_MODULE_ABI_VERSION
#define VENG_MODULE_ABI_VERSION 2u
#endif
```

`Module.h` now needs `TypeRegistry` visible — include
`<Veng/Reflection/TypeRegistry.h>` (a pure-CPU public header, no backend leak).
The `VengModuleAbiVersion()` handshake (`ModuleLoader::Load`) already rejects a
mismatch before `VengModuleRegister` runs, so a stale module surfaces as a
recoverable load error, not a crash — the existing `bad_version_module` test still
covers the mechanism, now against `2u`.

## Ownership reconciliation — registry moves host-side

Today `Application` owns `m_TypeRegistry` and `GetTypeRegistry()` returns it; the
launcher constructs the `Application` **after** calling `module->Register(host)`
(the app's factory is *obtained from* that call). A registry the module fills at
`Register` time therefore cannot live inside the not-yet-constructed app. The owner
moves up exactly one frame, to a launcher local sitting right beside the
`ApplicationRegistry` local — which already outlives the app and is the precedent
for "host-owned, threaded into the app." The move:

- **`Application` borrows the registry.** Drop the `TypeRegistry m_TypeRegistry`
  member; take a `TypeRegistry&` (stored as a reference/back-pointer) through the
  `Application` constructor / `ApplicationInfo`, the same explicit-threading
  discipline as `Context`/`AssetManager`/`TaskSystem`. `GetTypeRegistry()` returns
  the borrowed reference — **callers are unchanged**.
- **The host owns it.** The launcher constructs the `TypeRegistry`, pre-registers
  builtins, fills the host, calls `Register`, then constructs the app threading the
  registry in. (The exact constructor/`ApplicationInfo` shape settles against
  planset-10's landed `Application` API; the contract is: the registry is supplied,
  not owned.)

This **supersedes planset-10 decision 4** — record it in plan 05's docs pass. The
registry is now owned by whoever loaded the module (launcher here, cooker in plan
02), which is precisely what makes one registration path serve both.

> **Why borrow and not keep `Application` the value owner.** Preserving
> `TypeRegistry m_TypeRegistry` is possible but costs more: the module fills the
> launcher's local before the app exists, so handing that to a value-owning
> `Application` means either threading a `TypeRegistry&&` through the factory
> signature (rippling into `ApplicationRegistry` and every module's
> `RegisterApplication`) or a post-`Create` "adopt" step. The borrow is the lighter
> touch and matches `ApplicationRegistry`'s existing lifetime. A second C-ABI entry
> (`VengModuleRegisterTypes`) is rejected — game-module.md resolved "single
> `VengModuleRegister`, not multiple named exports."

## The GPU-free builtin-registration contract — engine

planset-10 pre-registers the builtins (`Name`, `Transform`, `Parent`, and — from
its plan 04 — `Camera`/`CameraComponent`, `MeshRenderer`) somewhere inside the app's
startup. Extract that into a **public, GPU-free** function the launcher and cooker
both call:

```cpp
// Pre-registers the engine's builtin reflected types (leaf vocabulary + builtin
// components) into a registry. GPU-free: no Context, no device — callable from a
// headless cooker. Idempotent per type (planset-10's auto-registration rule).
void RegisterBuiltinTypes(TypeRegistry& registry);
```

The launcher calls it on the host registry **before** `module->Register(host)`, so
a game component referencing a builtin leaf finds it already present (planset-10's
auto-registration on reference also covers ordering, so this is robustness, not a
strict requirement). The **GPU-free contract** is twofold and now *guaranteed*:
`RegisterBuiltinTypes` and `Register<T>` touch no device, and the `Application`
**factory lambda** registered through `host->App` constructs no GPU object (it
captures `ApplicationInfo` and defers all device work to `Run`/`OnInitialize`).

## The launcher — `engine/src/Launcher/launcher_main.cpp`

The launcher grows the registry it now owns:

```cpp
auto module = Veng::ModuleLoader::Load(VENG_GAME_MODULE);
if (!module) { /* unchanged error path */ }

Veng::ApplicationRegistry apps;
Veng::TypeRegistry        types;
Veng::RegisterBuiltinTypes(types);                       // GPU-free builtins first

Veng::VengModuleHost host{.App = apps, .Types = types, .Editor = nullptr};
module->Register(host);                                   // module registers its components here

Veng::Unique<Veng::Application> app = apps.Create();      // factory is GPU-free
VE_ASSERT(app, "module registered no Application");
// thread `types` into the app (borrowed) per the settled Application API
app->Run(Veng::vector<Veng::string>(argv, argv + argc));
```

Declaration order still matters: `module` first (destructs last), then the
registries, then the app — a registered factory and the registered descriptors are
code/data whose definitions live in the module image, so both registries and the
app must die before the module unloads. `types` and `apps` destruct before
`module`. Keep the existing comment's intent, extended to the registry.

## The sample's module — `examples/hello-triangle/`

planset-10 plan 04 registers the game's `Spinner` (and uses the public
`Register<T>` path) at app startup. Move that registration into the module's
`VengModuleRegister`:

```cpp
extern "C" void VengModuleRegister(Veng::VengModuleHost* host)
{
    host->Types.Register<HelloTriangle::Spinner>();   // game component, via VE_REFLECT
    host->App.RegisterApplication([] { /* unchanged factory */ });
}
VE_EXPORT_MODULE_ABI();
```

Builtins are already present (the launcher pre-registered them); the game adds only
its own types. The app's `OnInitialize` no longer registers components — it consumes
`GetTypeRegistry()` as before for `Scene::Create`.

## Tests

`tests/module/` + `veng_unit` (`-L unit`), `-L death` for misuse:

- **ABI bump:** `bad_version_module` (or its sibling) confirms an ABI-1 module is
  rejected by `ModuleLoader::Load` against the now-`2u` host — the handshake fires
  before `VengModuleRegister`.
- **Registration through the entry:** a `test_module` builds a host with a real
  `TypeRegistry`, runs `Register`, and asserts the module's component type is present
  in the registry with the expected descriptors (name/fields) — the launcher path in
  miniature.
- **Headless, no-device reflection (the new contract guard):** load the module and
  run `RegisterBuiltinTypes` + `Register` with **no `Context` constructed** and assert
  the types reflect correctly. Labelled so it runs with no Vulkan ICD (it needs
  none). This is the property plan 02's cooker depends on.
- **Builtins are GPU-free + idempotent:** `RegisterBuiltinTypes` on a fresh registry
  registers all builtins; called twice it is a no-op per type (no duplicate/collision
  assert).
- **Borrowed-registry lifetime:** the app uses `GetTypeRegistry()` and the registry
  outlives the app (death test: a stale registry reference is not the failure mode —
  the launcher's declaration order guarantees order; assert the order contract holds).

`include_hygiene`: `Module.h` now pulls `Reflection/TypeRegistry.h` (already in the
manifest); no new public header.

## Acceptance

Clean build; `ctest -L unit` + `-L death` + the module tests green; `include_hygiene`
builds; the launcher smoke (`hello_triangle_launcher_smoke`) still writes a correct-
sized PPM and exits 0 (the registration path moved but the render is unchanged).
Commit: `Plan 01: module TypeRegistry seam — VengModuleHost.Types, ABI v2, host-owned
registry, GPU-free RegisterBuiltinTypes`.
