# Build configurations & project settings — design overview (vision)

> **Delivered by [planset-35](../planset-35/README.md).** The developer-control layer this
> document designs — the project-settings / build-configuration concept, the role → format
> table, the coarse per-config cook dependency, the host-default CMake selection, and the
> editor host-capability preview gate — landed in planset-35 and is documented in the
> `CLAUDE.md` set (engine `Veng/Project/`, the cooker resolution + CMake selection, the editor
> Project Settings panel + preview gate). What remains future is the **footprint** work in
> "Open questions" below — each a new format/encoder or an orthogonal concern, not
> settings-tier work: **BC5/BC4 channel specialization**, **wider ASTC footprints**, **HDR
> ASTC**, an **uncompressed fallback pack**, **editor active-config persistence**, and the
> **Windows cross-compile constraint**. The role *taxonomy* is settled; its *per-codec
> specialization* is not — under the two current codecs every role maps full-channel
> (`Color`→sRGB, the rest→unorm), and the channel-specialized mappings (`Normal`→BC5,
> `Mask`→BC4) plus the ASTC normal-packing convention ride those footprint follow-ons.

> **Motivated by planset-33's texture-compression track.** That track ships the
> codec *plumbing* — BC7/ASTC formats, the `FormatInfo` block helper, the
> `textureCompressionBC` / `textureCompressionASTC_LDR` capability queries, the
> cooker encoders, the block-aware upload — and deliberately **hardcodes ASTC as the
> cook default** (BC7 selectable through a minimal internal seam), deferring all
> *developer control* of which codec a texture cooks to. This document is that deferred
> control layer. It is **not scheduled**; it becomes its own planset when taken up.
>
> **The gate is met by planset-33:** the formats, the per-format block math, the
> device-capability queries, and a cooker that selects a codec per texture all
> exist by the end of that track — this area only adds the *authoring* surface that
> chooses the codec, plus the project/build-settings concept that surface lives on.

## The problem

A texture's codec is **not** a per-asset decision the way `srgb` or `max_size` is —
it is a **platform** decision. BC7 needs `textureCompressionBC` (Apple-Silicon-only
under MoltenVK; the desktop/Windows standard); ASTC needs
`textureCompressionASTC_LDR` (broad on Apple GPUs). One project ships *several*
targets — Windows, macOS, Linux, mobile — each wanting a different codec for the
*same* source art. veng has **no home** for a per-platform build policy today: the
asset-pack manifest is a pure `{ id, type, source }` table by rule, and per-asset
`*.tex.json` sources are the wrong scope (the artist should not pick BC7 vs ASTC per
texture per platform).

## The three tiers

The decision factors into three scopes, resolved most-specific-wins:

```
  Project settings (one)
    └─ owns the list of build configurations + the editor's active/default one
       Build configuration (N: Windows / macOS / Linux / mobile / …)
         └─ owns the texture codec policy: a role → concrete-format table
            Per-asset *.tex.json (per texture)
              └─ declares a compression role/intent (Color / Normal / Mask / HDR / UI)
                 (a raw per-texture codec override is the escape hatch, not the norm)
```

- **Project settings** (one per project) — project-wide invariants: the **set of
  build configurations**, the **active/default** configuration the editor previews
  through, asset-id policy, content layout. No codec here.
- **Build configuration** (N) — a named **ship target**: a platform/target triple,
  the **texture codec policy** (the role → format table), compression level, shader
  profile, and the output pack path. **The codec lives here.** This is the "yet
  another concept, the build settings, of which there can be multiple
  configurations" the design calls for.
- **Per-asset role** (`*.tex.json`) — the texture declares a **compression
  intent/role** (Color / Normal / Mask / HDR / UI), **not a raw codec**. Each build
  configuration maps role → concrete format per platform (`Normal → BC5 on Windows,
  ASTC4x4 on mobile`). This is the standard cross-engine factoring (Unreal
  `TextureCompressionSettings`); it avoids a combinatorial per-texture-per-platform
  codec matrix and lets the artist declare *intent* while the platform decides the
  *format*. A raw `"codec"` override stays as an escape hatch for the rare case.

## Resolution & output

A texture's cooked format is `BuildConfig.RoleToFormat[ tex.Role ]`, unless the
texture pins a raw `"codec"` override. **One output pack per (pack × build
configuration)** — `vengc cook <manifest> --config <buildconfig> -o
<name>-<config>.vengpack` — so a project builds `sample-windows.vengpack`,
`sample-macos.vengpack`, etc. The shipped game loads the pack for its platform.

## The cook-time dependency model — implicit and coarse, by design

The build-time cook is **whole-pack**, not per-asset: `vengc cook` emits one
`.vengpack` plus a depfile of every source it read, and CMake/ninja re-cooks the
**entire pack** when any depfile entry changes ([`cmake/AssetPack.cmake`](../../cmake/AssetPack.cmake)).
There is no per-asset build-time cache to preserve. Therefore:

- **No fine-grained per-asset dependency edge is needed.** The cooker records the
  active build-configuration file as **one central cook input** — exactly as it
  already records the pack JSON centrally — so it lands in the depfile and a change
  re-cooks the pack. A per-importer `RecordDependency(configFile)` would buy nothing
  over this, because a config change re-cooks every texture anyway.
- **Per-config invalidation falls out for free.** Because each configuration is its
  own output pack with its own depfile, editing `windows.buildcfg` re-cooks only
  `sample-windows.vengpack` — the macOS pack's depfile does not name that file.
  There is no global mutable "active codec" to reason about; selecting a config is
  *selecting an output*, not invalidating a shared cache.
- **The only cost of coarseness** is that editing a config re-cooks the whole pack,
  not just its textures. The texture encode is the expensive, actually-changed part,
  and the rest of a pack re-cook is fast — so this is the right altitude. If it ever
  hurts, the fix is a **content-addressed per-asset cook cache** (a blob keyed on
  inputs + resolved config), which is a **general cooker optimization orthogonal to
  this design** — do not couple them.

## The editor — surfacing it, and gating it to the host GPU

Two editor concerns: *exposing* the settings, and *preventing* the editor from
trying to preview a format the host GPU cannot sample.

### Surfacing (cheap — reuse the reflection inspector)

`ProjectSettings` and `BuildConfiguration` are **reflected structs** (`VE_REFLECT`;
the codec/role fields are `VE_LEAF(..., FieldClass::Enum)` so the inspector draws
combos for them, as it already does for `LightType`). They are **JSON cook inputs**
the cooker hand-parses (like every other source), with the reflected schema giving
the editor its panel **for free** through the shared `DrawFieldWidget` /
`PropertyTable` — no new inspector machinery, the same pattern as
`LevelRenderSettings`. So:

- A **Project Settings panel** (host-level, opened from the Window menu like the
  asset browser) lists/edits the build configurations and the active one.
- The **texture editor** gains a **compression role** combo (defaulting to a guess
  from `srgb`/usage), writing/clearing the `*.tex.json` key over the existing
  unknown-key-preserving round-trip; it shows the *resolved* format read-only
  ("→ BC7 (Srgb) for active config 'Windows'").

### Gating preview to host capability — the structural guarantee

Encoding ASTC and *sampling* ASTC are different operations. The ASTC/BC encoders are
**CPU code** — cooking the mobile/ASTC pack on a Windows box is normal and must
**never** be blocked. What cannot happen on Windows is the editor's **live-preview
path** mounting an ASTC blob and handing it to a GPU that lacks
`textureCompressionASTC_LDR`. So the rule is:

| Path | Needs | Host constraint |
|---|---|---|
| **Build / ship** (CMake cook → output pack) | CPU encoder | none — build any config anywhere |
| **Editor live preview** (cook-on-demand → `MountMemory` → sample) | GPU can sample the format | **must match host capability** |

- **Editing a config is always allowed** (author the mobile/ASTC config on Windows);
  only **previewing/playing *through* it** is gated.
- **Gate on device features, not platform labels.** The editor queries the host
  `Context` for `IsBlockCompressionSupported()` / `IsAstcSupported()` (the planset-33
  queries) and intersects a config's resolved formats with them. Gate on the
  *feature*, not a "macOS" string — an Intel Mac (no BC) and an Apple-Silicon Mac (BC
  + ASTC) are both "macOS" but differ in exactly the bit that matters.
- **Default preview is host-derived, so the bad path is structurally impossible.**
  The editor's default live-cook target is a **host-safe** profile (uncompressed, or
  the host's best-supported codec), *independent of which ship config is selected for
  editing*. The editor therefore never hands the GPU an unsamplable blob by accident.
- **"Preview as ship config" is opt-in** WYSIWYG (to eyeball BC7/ASTC artifacts), and
  the preview selector **disables host-incompatible configs** with a reason — e.g.
  *"macOS-Mobile (ASTC4x4) — not previewable: this GPU lacks ASTC. Build-only."*
- **Fallback so the editor is never stuck.** If no configuration is host-previewable
  (a mobile-only project opened on a BC-less GPU), the editor previews host-safe with
  a banner — *"previewing uncompressed; no build configuration targets this GPU"* —
  and stays fully editable.

The same `IsAstcSupported()`-style check is both the **capability warning** ("active
config not supported on this GPU") and the **preview-eligibility gate** — one query,
two uses. The editor live-edit invalidation is the per-asset counterpart to the
build-time coarse rule: when the active preview config changes, the editor drops the
affected texture handles and re-runs cook-on-demand for the visible ones against the
new (host-safe) format, re-mounting behind the stable handles — the existing
recook-and-hot-reload path, triggered by a config change instead of a source edit.

## Wiring sketch

- **`ProjectSettings` / `BuildConfiguration`** — reflected structs (engine-side, e.g.
  `Veng/Project/`), serialized as JSON cook inputs. The cooker hand-parses them; the
  editor draws them through reflection.
- **`CookContext`** gains a `const BuildConfiguration* Config = nullptr` (alongside
  `Types` / `Systems`); the `TextureImporter` resolves `role → format` through it,
  with the per-texture raw override on top. The cooker records the config file as a
  central depfile input.
- **`vengc cook … --config <file>`** selects the configuration; `add_asset_pack`
  grows a config dimension (one output pack per configuration). The editor's
  cook-on-demand passes the active config (host-clamped for preview).
- **Host capability** — reuse `Context::IsBlockCompressionSupported()` /
  `IsAstcSupported()` from planset-33 for the preview gate and the warning.

## Decisions settled

1. **Codec is a build-configuration property, not a project default or a per-asset
   knob.** Per-platform by nature; the project owns the *list* of configs, the config
   owns the codec policy.
2. **Per-asset declares a role, not a raw codec.** The build config maps role →
   format per platform; a raw override is an escape hatch.
3. **The build-time config dependency is implicit and coarse** — a central depfile
   input over a whole-pack cook, one output pack per config. No fine-grained per-asset
   edges; the content-addressed per-asset cook cache is a separate, orthogonal cooker
   optimization.
4. **The editor previews host-safe by default and gates preview-through-config on
   device features.** Building any config is unrestricted; previewing one is bounded
   by what the host GPU can sample. The failure ("ASTC on Windows") is made
   structurally impossible, not merely warned against.

## Open questions (settle when taken up)

- **Role taxonomy** — the exact closed set (Color / Normal / Mask / HDR / UI / …) and
  each role's per-codec format (e.g. `Normal → BC5` vs `ASTC4x4` — ASTC has no
  two-channel mode, so a normal map under ASTC needs a packing convention).
- **Default config / no-config** — what a pack with no project settings cooks to (the
  planset-33 hardcoded ASTC default stays the zero-config default).
- **Footprint specialization** — the still-open codec footprints the role table would
  pick among: **BC5/BC4 channel specialization** (two-channel normals / single-channel
  masks rather than full BC7); **wider ASTC footprints** (6×6, 8×8 — more compression,
  lower quality, a per-role choice); and **HDR ASTC** (the cooked codec is LDR-only;
  HDR sources have no compressed path yet — environments keep their `RGBA16Sfloat`
  panorama).
- **Uncompressed fallback pack** — a config (or a fallback within one) targeting a device
  that supports neither cooked codec, so a texture is samplable instead of
  `AssetError::Unsupported`.
- **Editor active-config persistence** — per-project vs per-user editor state.
- **The Windows cross-compile constraint** — building a foreign-platform pack is
  CPU-only and fine, but the `--module` prefab reflection still loads a *host* lib
  (area 10's recorded cross-compiled-cooking constraint), so a fully cross-compiled
  build target inherits that latent limit.
