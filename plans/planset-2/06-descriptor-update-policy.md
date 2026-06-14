# 06 — Descriptor binding flags & update policy (addendum)

> **Addendum, done (2026-06-13).** Not part of the original planset-2 scope.
> It captures a design finding surfaced once the `VE_DEBUG` validation build was
> repaired (planset-2 follow-up): the descriptor-set abstraction hard-coded
> Vulkan descriptor-indexing flags on *every* binding, and that hard-coding had
> silently drifted out of sync.
>
> **Implemented as recommended below (parts 1+2; part 3's stopgap is subsumed
> by part 1):**
> - `DescriptorBinding::Bindless` (default `false`) — static bindings get no
>   descriptor-indexing flags at all.
> - `GetDescriptorTypeInfo(DescriptorType)` in `TypeMapping.h` is the single
>   table: Vulkan type, Primary Pool budget, and `SupportsBindless`. A
>   `Bindless = true` binding on a type with `SupportsBindless == false` is a
>   named `VE_ASSERT`. `DescriptorSetLayout` only sets
>   `ePartiallyBound | eUpdateAfterBind | eUpdateUnusedWhilePending` (and the
>   layout's `eUpdateAfterBindPool` flag) for bindings that opt in.
> - `Context::CreateDevice` now also enables
>   `descriptorBindingStorageImageUpdateAfterBind`; the Primary Pool sizes a
>   `vk::DescriptorPoolSize` for every `DescriptorType` (was missing
>   `SampledImage`/`StorageImage`).
> - The storage-image validation gap (CLAUDE.md, `cmake/ValidationGate.cmake`)
>   is closed — the gate's allowlist is now empty.

**Goal:** decide how `UPDATE_AFTER_BIND` (and the other descriptor-indexing
binding flags) are governed — and fix the underlying coupling bug — without
leaking Vulkan onto the consumer.

**Dependencies:** none new. Touches the descriptor types delivered in planset-1
(typed descriptor writers).

## The finding

Running the validation build (now that it compiles) shows the `compute_dispatch`
storage-image path is **not** validation-clean:

```
ERROR vkCreateDescriptorSetLayout: pBindingFlags includes UPDATE_AFTER_BIND_BIT
      but descriptorType is STORAGE_IMAGE and
      descriptorBindingStorageImageUpdateAfterBind was not enabled.
WARN  vkAllocateDescriptorSets: pool has no STORAGE_IMAGE pool size.
```

The original triangle/sampled-image path is clean; only storage images break.

### Root cause: one policy, three hand-maintained lists, drifted

`DescriptorSetLayout` ([`DescriptorSetLayout.cpp:37`](../../src/Renderer/Backend/DescriptorSetLayout.cpp))
applies, to **every** binding unconditionally:

```cpp
ePartiallyBound | eUpdateAfterBind | eUpdateUnusedWhilePending
// + layout flag eUpdateAfterBindPool
```

`UPDATE_AFTER_BIND` is not free: each descriptor type used with it requires a
matching device feature enabled at device creation, and the pool must carry that
type's budget and the `eUpdateAfterBind` pool flag. Those prerequisites live in
two *other* places, and the three lists no longer agree:

| veng `DescriptorType` | Pool size (`Context.cpp:240`) | UAB feature (`Context.cpp:544`) | Layout flags |
|---|:---:|:---:|:---:|
| UniformBuffer | ✓ | ✓ | UAB (all) |
| StorageBuffer | ✓ | ✓ | UAB (all) |
| CombinedImageSampler | ✓ | ✓ (via SampledImage) | UAB (all) |
| SampledImage | ✗ | ✓ | UAB (all) |
| **StorageImage** | **✗** | **✗** | UAB (all) |

`StorageImage` is missing on both axes (hence the error); separate `SampledImage`
would fail on the pool too. The abstraction doesn't leak through its API — it
leaks as a validation error the first time an un-pre-wired type is used. That is
the "built for the basic case, over-generalized the flags" smell, exactly.

## The question: should `UPDATE_AFTER_BIND` be caller-defined?

**The instinct is right, the literal fix isn't.** Hard-coding UAB on everything
is wrong — but exposing the *raw* `UpdateAfterBind` flag to the consumer is the
wrong altitude and doesn't actually fix the problem:

- It leaks Vulkan descriptor-indexing semantics (`partiallyBound`,
  `updateUnusedWhilePending`, "after bind") onto callers who shouldn't reason
  about them — against veng's whole "say intent once, derive the Vulkan" stance
  (typed push constants, render-graph-derived barriers, typed descriptor writes).
- Its prerequisites are **engine-owned**: setting the flag is only valid if the
  engine *separately* enabled the device feature and sized the pool for that type
  — which the caller can't see or control. So a caller-set flag is a footgun: it
  compiles, then trips the same validation error. Control without the matching
  control over features/pool is illusory.

What the caller legitimately knows is **intent**, not flags: "this is a fixed
binding I write once and bind" vs. "this is a bindless table I stream into while
it may be in flight." Expose the intent; let the engine pick the flags.

## Recommendation

Three parts — a target design, the coupling fix that makes it robust, and an
immediate stopgap.

### 1. Static by default, bindless by intent (target design)

Most bindings are written at setup (or between frames) and then bound — they need
**none** of `UpdateAfterBind` / `partiallyBound` / `runtimeDescriptorArray`.
Make that the default:

- **Static binding** → plain flags, no descriptor-indexing features required.
  This is the common case and the most portable one. It also fixes the finding
  outright: a *static* `StorageImage` binding sets no UAB, so the absent
  `…StorageImageUpdateAfterBind` feature is irrelevant.
- **Bindless binding** → opt in with a high-level, veng-vocabulary field on
  `DescriptorBinding`, e.g. `bool Bindless = false` (not `eUpdateAfterBind`).
  Only a bindless binding turns on UAB + `partiallyBound` + (for arrays)
  `runtimeDescriptorArray`. This is the path `WriteArray` and any ImGui-style
  texture table actually need; the heavy flags become justified and localized.

This mirrors the render graph: declare *what* (Color/Sample/StorageWrite), the
engine derives the Vulkan. Here you declare *static vs bindless*, the engine
derives the binding flags.

> Note: veng's own `DescriptorSet::Write` updates host-side and the sets in the
> sample are written once at init, so the current code does **not** actually
> require UAB for its basic usage — the universal flag is "just in case"
> generality. Static-by-default removes a requirement that was never needed.

### 2. One source of truth for the type ↔ feature ↔ pool coupling

The bug exists because the three lists above are maintained by hand in three
files. Centralize a single table keyed by veng `DescriptorType` giving its
`vk::DescriptorType`, its pool budget, and the device feature it requires *when
used bindless*. Drive device-feature enabling, Primary-Pool sizing, and bindless
flag application from that one table. Then:

- Adding a `DescriptorType` updates one place; the lists can't silently desync.
- Requesting `Bindless` on a type whose feature isn't available fails with a
  named engine assert ("StorageImage bindless needs
  descriptorBindingStorageImageUpdateAfterBind") instead of a raw Vulkan message.

### 3. Immediate stopgap (ship first, independently)

Until the redesign lands, make the *current* universal-UAB scheme coherent so the
validation build is clean today: enable `descriptorBindingStorageImageUpdateAfterBind`
and add `eStorageImage` (+ `eSampledImage` for completeness) pool sizes. ~4 lines,
no API change. The redesign then *reduces* the enabled-feature surface again by
making static the default. (This is the only part safe to do before review.)

## Acceptance

- A static `StorageImage` binding (the `compute_dispatch` test) is
  validation-clean **without relying on update-after-bind**.
- Bindless/array bindings (`WriteArray`, ImGui-style tables) still work via the
  intent opt-in, and their feature/pool prerequisites are satisfied.
- Device features, pool sizes, and binding flags all derive from one table;
  adding a `DescriptorType` is a one-place change.
- Misuse (bindless on an unsupported type) is a named engine assert, not a raw
  validation error.
- No raw Vulkan descriptor-indexing flag appears on the public descriptor API.

## Out of scope

A full bindless / descriptor-indexing subsystem (large arrays, per-frame
streaming, descriptor buffers) belongs to the asset/material phase drafted in
[future/](../future/README.md). This addendum only corrects the flag-policy
altitude and the coupling bug; it deliberately keeps the bindless opt-in minimal.
