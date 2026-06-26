# Plan 06 — docs + roadmap

**Goal:** document the build-configuration system and the minimal template across the `CLAUDE.md` set
and the root, mark future [area 15](../future/build-configurations.md) **delivered** (its footprint
items staying future), and run the full verification band. The closer. **Depends on Plans 00–05.**

## Why it is its own plan

The planset introduces a genuinely new concept — **project settings / build configurations** — and a
second canonical example, both of which several module guides must describe accurately. Folding the
documentation into the implementation plans would scatter it; a single closer that writes it once,
against the landed code, keeps the guides coherent and lets the implementation plans stay focused on
their seams.

## What lands

- **Root `CLAUDE.md`.** Document the build-configuration model in the cooker/asset narrative: a texture
  declares a **role**, a **build configuration** owns the role → format table, the cook reads the
  active configuration (`vengc cook --config`) and emits **one pack per configuration**, and a bare
  `cmake --build` cooks the **host-matching configuration** by default (`VENG_BUILD_CONFIG`, host-triple
  default; `cook-all-packs` for all). Record the **two co-migrated examples** rule in "Working norms":
  a breaking change migrates `examples/hello-triangle` *and* `examples/template` in the same pass.

- **`cooker/CLAUDE.md`.** The `CookContext.Config` seam, the role → format resolution (raw `codec` >
  role > zero-config ASTC), the config file as a central depfile input, and one output pack per
  configuration. Note the zero-config default is preserved (a pack with no project settings cooks as
  planset-33 left it).

- **`editor/CLAUDE.md`.** The Project Settings panel (reflection-driven, Window menu), the texture
  editor's role combo + resolved-format read-out, and the **host-capability preview gate** (build any
  configuration; preview only what the host GPU can sample; "ASTC on Windows" structurally
  impossible). Note the gate reuses planset-33's `IsBlockCompressionSupported()`/`IsAstcSupported()`.

- **`engine/CLAUDE.md`.** The new `Veng/Project/` home (the reflected `ProjectSettings` /
  `BuildConfiguration` / `CompressionRole`), and the minimal `examples/template` as the smallest app +
  the copy-to-start starter.

- **`future/README.md` + `future/build-configurations.md`.** Mark **area 15 delivered** by
  planset-35: the project-settings/build-configuration concept, the role → format table, the coarse
  per-config cook dependency, the host-default CMake selection, and the editor host-capability gate.
  Keep only the **still-future remainder** — the footprint items (**BC5/BC4 channel specialization**,
  **wider ASTC footprints**, **HDR ASTC**, the **uncompressed fallback pack**), **editor active-config
  persistence**, and the **Windows cross-compile constraint** — each a new format/encoder or an
  orthogonal concern, not settings-tier work. Update the area-15 line in `plans/README.md`'s future
  recap accordingly.

- **`plans/README.md`.** Add the planset-35 index entry (build configurations + the minimal template).

## Decisions

1. **Documentation lands once, against the landed code.** A single closer writes the guides after the
   seams are real, rather than each plan re-describing the system.
2. **Area 15 is delivered, its footprint items are not.** The *developer-control layer* (settings
   tiers, role/format table, host-default cook, preview gate) ships; the *new codecs/footprints*
   (BC5/BC4, wider ASTC, HDR ASTC, fallback pack) stay named follow-ons behind the delivered role →
   format table.
3. **The two-examples rule is a documented norm.** The co-migration obligation lives in the root
   `CLAUDE.md` "Working norms," so it binds future plansets, not just this one.

## Files

| File | Change |
|---|---|
| `CLAUDE.md` | Build-configuration model + the host-default cook + the two-examples co-migration norm. |
| `cooker/CLAUDE.md` | `CookContext.Config`, role → format resolution, central depfile input, one pack per config. |
| `editor/CLAUDE.md` | Project Settings panel, role combo, the host-capability preview gate. |
| `engine/CLAUDE.md` | `Veng/Project/` types; `examples/template` as the minimal starter. |
| `plans/future/README.md` | Mark area 15 delivered; keep the footprint/persistence/cross-compile remainder. |
| `plans/future/build-configurations.md` | A delivered banner; the open questions that remain future. |
| `plans/README.md` | The planset-35 index entry; update the area-15 future recap. |
| `plans/planset-35/README.md` | Flip every plan's status to `done`. |

## Verification

- The **full band** green: clean `cmake -B build` + build; `ctest --test-dir build
  --output-on-failure` (including `template_launcher_smoke` and `hello_triangle_launcher_smoke`);
  `smoke_golden` unmoved (the macOS configuration cooks the same ASTC bytes); `ctest -L validation` on
  a `build-debug` (`validation_gate` green); `include_hygiene` green.
- A `-DVENG_BUILD_CONFIG=windows` configure cooks the BC7 pack (build-only); `cook-all-packs` builds
  every configuration's pack.
- The `#embed` core-pack staleness footgun ([[project_ccache_embed_staleness]]) is not triggered (no
  core-pack format change this planset), but the band is run from a clean `build/` to be sure.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
