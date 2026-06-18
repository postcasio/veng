# planset-16 — grab bag: reflection cleanup, asset-library rename, ImGui composite

**Phase goal:** a **grab-bag** planset (like planset-7's utility cadence) — a set of small,
self-contained wins that don't each warrant a planset of their own, bundled into one. It carries
no single theme; each plan stands alone, and the plans are largely independent of one another.

The wins:

1. **Unify the reflection identity trait.** Collapse the reflection layer's **two** type-identity
   traits — `ReflectLeaf<T>` (leaves: scalars, vectors, enums, `AssetHandle<T>`, `Entity`) and
   `VengReflect<T>` (structs/components, via `VE_REFLECT`/`VE_TYPE`) — into a **single
   `VengReflect<T>`** every reflected type specialises uniformly, deleting the
   `Detail::IsReflectLeaf` SFINAE, the parallel `TypeRegistry::EnsureLeaf<T>()` path, the
   leaf-vs-struct branch in dependency auto-registration, and the `Class`-member-vs-`Class()`
   inconsistency. **Behaviour-preserving** — no on-disk format, `TypeId`, or `FieldClass` change
   (plans 00 + 01).
2. **Rename `assetformat` → `assetpack`.** A mechanical rename of the shared archive/cooked-blob
   library — directory, CMake target + `veng::` alias, comments. No header path, API, or
   `.vengpack` format change (plan 02).
3. **`ImGuiCompositePass`.** An engine-provided helper owning the scene-output → ImGui plumbing
   every `SceneRenderer` app hand-writes today (the ImGui scene texture + the pre-ImGui barrier,
   plus the scene-behind-ImGui composite in composite mode); hello-triangle and the editor's scene
   panels migrate onto it. Realizes [future area 11](../future/README.md#11-imgui-composite-pass--done-planset-16) (plan 03).

None of these is part of a [future-area](../future/README.md) chain that needs a planset to
itself; bundling them keeps the per-planset cadence without three near-empty plansets.

## Win 1 — unify the reflection trait (plans 00 + 01)

### The finding

The reflection layer carries two traits that both answer "what is this type's stable
`TypeId`?":

- `ReflectLeaf<T>` — `{ Id, Class }`, where `Class` is a *non-`Struct`* `FieldClass`.
  Specialised for the types you **cannot** put a `VE_REFLECT` body inside: the builtin
  scalars/vectors (`f32`, `vec3`, `quat`, `mat4`, …), `string`, `AssetHandle<Texture|Mesh|
  Material>`, `Entity` (a `Reference` leaf), and game enums (`reflection.cpp`'s
  `ReflectLeaf<Team>` with `Class = Enum`).
- `VengReflect<T>` — `{ Id, Name(), Class()=Struct, Fields(), Describe(),
  RegisterDependencies() }`, written by `VE_REFLECT` (fielded) or, id-only, by `VE_TYPE`.

A `Detail::IsReflectLeaf<T>` `void_t` detector (`decltype(ReflectLeaf<T>::Id)`) drives the
two compile-time dispatchers (`TypeIdOf<T>()` / `FieldClassOf<T>()`) to read from one trait
or the other, and `TypeRegistry` grows a parallel `EnsureLeaf<T>()` next to `Register<T>()`
for the leaf branch (the dependency walker in `Reflect.h` picks between them on
`FieldClassOf<Field>() == Struct`).

**The split is redundant.** The real "recurse into fields or not?" discriminator is already
`FieldClass` (`Struct` vs. everything else) — the dependency walker switches on it directly,
not on which trait `T` uses. So `IsReflectLeaf` is a *second* encoding of the same
Struct-vs-not distinction. Worse, `VE_TYPE` already blurs the partition: it writes a
fieldless type into `VengReflect<T>` (so it classes as a `Struct`), yet its own doc comment
calls it "a game leaf that needs an id but no fields" — so the partition is not really
leaf-vs-struct, and the `ReflectLeaf` name overreaches (it also holds enums). The only thing
the two-trait split buys is that asking a leaf for `Fields()` fails to compile; the merge
recovers that safety structurally (a leaf's `Fields()` is a trait-provided empty list — see
decision 2).

### Decisions

1. **One trait: `VengReflect<T>`.** `ReflectLeaf<T>` is removed. Every reflected type — leaf,
   id-only struct, or fielded struct — specialises `VengReflect<T>` with the **same member
   set**: `static constexpr TypeId Id`, `static constexpr FieldClass Class`,
   `static string Name()`, `static vector<FieldDescriptor> Fields()`, and
   `static void RegisterDependencies(TypeRegistry&)`. With the member set uniform,
   `TypeIdOf<T>()` and `FieldClassOf<T>()` become **direct reads** (`VengReflect<T>::Id` /
   `::Class`) with **no SFINAE**, and `Detail::IsReflectLeaf` is deleted.

2. **Leaves provide a trivial `Fields()`/`RegisterDependencies()` via a new `VE_LEAF` macro.**
   `VE_LEAF(Type, TypeIdLiteral, FieldClassValue)` writes the full uniform `VengReflect<Type>`
   for a non-struct leaf: the given `Id`/`Class`, `Name()` = `#Type`, `Fields()` = `{}`, and a
   no-op `RegisterDependencies`. The engine's builtin leaf vocabulary (the `ReflectLeaf<…>`
   block in `TypeId.h`) is reauthored through `VE_LEAF`; a game adds a leaf or enum the same
   way — `VE_LEAF(MyEnum, 0x…ULL, FieldClass::Enum)` — with no engine change, exactly as
   today. Because every specialisation now has `Fields()`/`RegisterDependencies()`, the
   zero-arg `Register<T>()` is fully uniform and **`EnsureLeaf<T>()` is deleted**: dependency
   auto-registration calls `Register<Field>()` for *every* field, leaf or struct (a leaf's
   empty `Fields()` bottoms out the recursion; the `contains()` guard stops self-cycles). This
   also gives leaves a real `TypeInfo.Name` (`"f32"`, `"vec3"`, …) instead of the empty string
   `EnsureLeaf` recorded — a small editor/log improvement that falls out for free.

3. **`Class` is a `constexpr` member everywhere, never a function.** `VE_REFLECT` and `VE_TYPE`
   currently spell it `static constexpr FieldClass Class() { return Struct; }`; that becomes a
   `static constexpr FieldClass Class = FieldClass::Struct;` member, matching the leaves. The
   member/function inconsistency that forced the two dispatchers to special-case is gone.

4. **`VE_TYPE` gains the uniform members.** An id-only struct/component (used by the ECS tests
   for poolable fieldless types) keeps its one-line authoring surface, but the macro now emits
   the full member set — `Class = Struct`, `Name()` = `#Type`, `Fields()` = `{}`, no-op
   `RegisterDependencies` — so it flows through the same uniform `Register<T>()` as everything
   else. The collision-assert behaviour (two types claiming one id) is unchanged (the
   `death_main.cpp` `CollideA`/`CollideB` test still fires).

5. **Behaviour-preserving, by contract.** No `TypeId` changes (every existing leaf id is
   carried over verbatim), no `FieldClass` change, no serializer change — `Serialize.cpp`
   still reads each leaf's `Size` off its registered `TypeInfo`, which the unified
   `Register<T>()` records identically to the old `EnsureLeaf`. The cooker's `PrefabImporter`
   and the engine's `PrefabLoader` keep resolving `TypeIdOf<AssetHandle<…>>()` /
   `TypeIdOf<f32>()` etc. unchanged (only the trait they read *through* changes, not the
   value). On-disk prefabs/materials are byte-identical before and after.

6. **Considered, not chosen: keep one SFINAE.** An alternative merge keeps a single
   `HasFields<T>` detector and lets leaves stay `{ Id, Class }` (no `Fields()`), with
   `Register<T>()` branching `if constexpr (HasFields<T>)`. It trades one SFINAE for another
   and leaves the blank-leaf-name behaviour in place. Decision 2's zero-SFINAE shape is
   preferred: no metaprogramming detector at all, and leaves gain real names. (Recorded so the
   reviewer sees the fork.)

### Scope

| In scope | Out of scope |
|---|---|
| Delete `ReflectLeaf<T>` + `Detail::IsReflectLeaf`; single `VengReflect<T>` trait | Any `TypeId` / `FieldClass` / on-disk format change |
| `VE_LEAF` authoring macro; reauthor the builtin leaf vocabulary through it | New reflected types, components, or leaves |
| Delete `TypeRegistry::EnsureLeaf<T>()`; uniform `Register<T>()` for leaf + struct | Reworking the `Register<T>(name)` / `Register<T>(name, cls, fields)` overloads (kept) |
| `Class` as a `constexpr` member in `VE_REFLECT`/`VE_TYPE`; direct-read dispatchers | Editor inspector / serializer logic (unchanged) |
| Migrate the one game-side leaf spec (`reflection.cpp` `ReflectLeaf<Team>` → `VE_LEAF`) | New `AssetId`s / new packs |
| Update reflection-layer contract comments to the single-trait model | — |
| Verify: clean build, full `ctest` green (unit/death/cooker/gpu), smoke PPM unchanged | — |
| Refresh `CLAUDE.md` reflection paragraph + `plans/README.md` | — |

## Win 2 — rename `assetformat` → `assetpack` (plan 02)

A mechanical rename of the shared archive/cooked-blob library. `assetformat` undersells what it
owns — not just a serialization layout but the asset identity types, the archive container, and the
cooked-blob definitions: the Vulkan-free contract shared by cooker and engine. `assetpack` names it
after the `.vengpack` artifact the project already ships. The `Veng/Asset/` include path is
**unchanged** (it is the namespace path, not the library name), so no `#include` lines move — only
the directory, the CMake target `libveng_assetformat` → `libveng_assetpack`, the `veng::assetformat`
→ `veng::assetpack` alias, and the comments naming the library. No API or on-disk format change.
Full detail in [plan 02](02-rename-assetformat.md).

## Win 3 — `ImGuiCompositePass` (plan 03)

An engine-provided helper that owns the scene-output → ImGui plumbing every `SceneRenderer` app
hand-writes today — the ImGui scene texture, the resize re-fetch, and the explicit
pre-`ImGuiLayer::Render` `PrepareForAccess(Sample)` barrier (universal), plus the fullscreen
composite pass and its three bindless registrations (composite mode) — behind `Create` /
`SetSource` / `Compile` / `PrepareSceneForImGui` / `GetSceneTexture`. It has two modes: **composite**
(scene → swapchain behind the ImGui overlay — hello-triangle) and **panel-only** (scene inside an
`ImGui::Image` — the editor's `SceneViewportPanel` and `MaterialPreview`). The new `composite.frag`
moves into the engine core pack under an engine-owned `AssetId` (the VS reuses the existing
`fullscreen.vert`); the three consumers migrate onto it and delete their hand-written copies. The
editor's `EditorHost::BuildPresentGraph()` (a plain ImGui-only swapchain blit) is a different
pattern and is **not** in scope. Realizes [future area 11](../future/README.md#11-imgui-composite-pass--done-planset-16).
Full detail in [plan 03](03-imgui-composite-pass.md).

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | [Unify the reflection trait](00-unify-reflect-trait.md) | Collapse `ReflectLeaf<T>` into `VengReflect<T>`: uniform member set, `VE_LEAF` macro, direct-read `TypeIdOf`/`FieldClassOf`, delete `IsReflectLeaf` + `EnsureLeaf`, `Class` as member. Migrate engine builtins + cooker + tests. Behaviour-preserving. | done |
| 01 | [Docs + roadmap re-cut](01-docs-roadmap.md) | `CLAUDE.md` reflection paragraph (single trait, `VE_LEAF` as the leaf/enum authoring surface), `plans/README.md` entry, this status table. No code. | done |
| 02 | [Rename `assetformat` → `assetpack`](02-rename-assetformat.md) | Mechanical rename of the shared archive/cooked-blob library: directory, CMake target + `veng::` alias, and comments. No header path, API, or `.vengpack` format change. | done |
| 03 | [`ImGuiCompositePass`](03-imgui-composite-pass.md) | Engine-provided scene-behind-ImGui composite helper (`Create`/`SetSource`/`Compile`/`PrepareSceneForImGui`/`GetSceneTexture`); composite shaders to the core pack; hello-triangle + editor migrated off their hand-written copies. Realizes future area 11. | done |

## Dependency analysis

This is a grab bag — **the three wins are mutually independent** (they share no files), so the only
ordering constraint is *within* win 1.

- **Win 1 (plans 00 → 01)** is **strictly sequential** (01 documents what 00 lands). 00 is a single
  coherent refactor across `engine/include/Veng/Reflection/{TypeId,Reflect,TypeRegistry}.h`,
  `engine/src/Reflection/Serialize.cpp` (no logic change; comment touch-ups only), the cooker's
  `PrefabImporter.cpp` comment, and the `tests/unit/{reflection,scene_ecs,scene_queries}.cpp` +
  `tests/death/death_main.cpp` suites — it does not subdivide cleanly into parallel work, and it is
  small. Run it inline.
- **Win 2 (plan 02)** touches the asset-library shell (directory + CMake target/alias) and comments.
- **Win 3 (plan 03)** touches the renderer, the core pack, and its three consumers — hello-triangle
  (composite mode) and the editor's `SceneViewportPanel` + `MaterialPreview` (panel-only mode).

Because the three wins are independent, they can land in any order, and 02 / 03 are safe to fan out
to worktrees in parallel if desired. Each is small enough to run inline.

**Shared doc files across wins (no shared source):** the only overlap between wins is in the docs.
Both plan 01 (win 1) and plan 02 (win 2) edit `CLAUDE.md` and `plans/README.md` — plan 01 adds the
reflection paragraph and the new planset-16 index entry; plan 02 renames `assetformat` prose mentions
and updates two existing index entries' library references. The edits are in different sections / on
different lines and do not overlap, but landing 01 and 02 in **parallel worktrees** would produce a
trivial merge in those two files. Since plan 01 is already sequenced after plan 00 (it documents
win 1), the natural order **00 → 01 → 02/03** avoids the conflict entirely; only force-parallelizing
01 and 02 incurs the merge. **No source file is shared between the three wins.**

## Process & conventions

Same cadence as every planset: implement → migrate any caller in the same pass → verify
(clean build, `ctest` green across the unit/death/cooker bands and the `gpu` band where a
device is present, smoke PPM correct size + exit 0) → update this table → one commit per plan.

The bullets below pin **win 1**; wins 2 and 3 each carry their own verification in their plan
files. Common to all three: the smoke PPM stays correct size + exit 0, and `include_hygiene`
stays green.

- **Behaviour-preserving is the bar.** The reflection unit tests (`tests/unit/reflection.cpp`)
  already pin `TypeIdOf`/`FieldClassOf`/`Fields()` for the builtins, a game leaf, and nested
  structs; they must pass **unchanged in intent** (the `ReflectLeaf<Team>` spec is the one
  edit — it becomes a `VE_LEAF` line — and its asserted id/class are identical). A diff to the
  on-disk encoding is a regression, not an accepted change.
- **`include_hygiene` stays green.** The reflection headers remain backend-free; the trait
  merge pulls in nothing new.
- **Contract comments are present-tense facts** — the trait paragraph in `TypeId.h` is
  rewritten to describe the single trait, with no "used to be two traits" narrative
  (CLAUDE.md comment policy).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

Three independent small wins have landed:

- **Win 1.** The reflection layer has **one** type-identity trait. A reflected type — a builtin
  scalar, a glm vector, an enum, an `AssetHandle`, an `Entity` reference, a fieldless component, or
  a fielded struct — specialises `VengReflect<T>` through one of three authoring macros (`VE_LEAF` /
  `VE_TYPE` / `VE_REFLECT`) that all emit the same member set, and the registry has one registration
  path (`Register<T>()`) instead of `Register` + `EnsureLeaf`. The `IsReflectLeaf` SFINAE and the
  `Class`-member-vs-function split are gone; on-disk format, `TypeId`s, and `FieldClass` are
  untouched.
- **Win 2.** The shared archive/cooked-blob library is `libveng_assetpack` / `veng::assetpack`; no
  `assetformat` token survives, and the `Veng/Asset/` include path and `.vengpack` format are
  unchanged.
- **Win 3.** `ImGuiCompositePass` ships in `libveng` with composite + panel-only modes;
  hello-triangle and the editor's `SceneViewportPanel` + `MaterialPreview` consume it and no longer
  hand-write the ImGui scene texture, the resize re-fetch, the pre-ImGui barrier, or (hello-triangle)
  the composite pass + bindless registrations; the new `composite.frag` lives in the core pack (the
  VS reuses the existing `fullscreen.vert`). `EditorHost`'s plain ImGui-only present blit is left as-is.

The grab bag pays down three structural seams without three near-empty plansets.
