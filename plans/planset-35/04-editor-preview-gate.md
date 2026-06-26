# Plan 04 — editor: gate live preview to host GPU capability

**Goal:** make "preview an ASTC blob on a GPU that cannot sample ASTC" **structurally impossible**.
The editor's default live-cook target is **host-safe** (independent of the selected ship
configuration); "preview as ship config" is opt-in and **disables host-incompatible configurations
with a reason**; a fallback banner covers the case where no configuration is host-previewable.
Building any configuration stays unrestricted — only previewing *through* one is gated. **Depends on
Plan 03.**

## Why it is its own plan

Encoding ASTC and *sampling* ASTC are different operations. Plan 03 surfaced the settings and routed
the active configuration into the editor cook; this plan adds the one rule that keeps the editor from
handing the GPU a blob it cannot sample. It is a focused capability-gating pass over the cook-on-demand
preview path, cleanly separable from the surfacing below it, and it is where planset-33's device
queries earn their second use (the warning *and* the gate are one query).

## What lands

- **A host-safe default live-cook target.** The editor's cook-on-demand preview (the
  `RequestCook` → `MountMemory` → sample path) targets a **host-safe profile** — uncompressed, or the
  host's best-supported codec — chosen from `Context::IsBlockCompressionSupported()` /
  `IsAstcSupported()`, **independent of which ship configuration is selected for editing**. So the
  editor never hands the GPU an unsamplable blob by accident: authoring the macOS/ASTC configuration on
  a BC-only Windows box previews host-safe, while the *build* of that configuration (CPU encode) stays
  unrestricted.

- **Capability intersection.** A small helper intersects a `BuildConfiguration`'s resolved formats
  with the host's enabled features (`IsBlockCompressionSupported()` for `BC*`, `IsAstcSupported()` for
  `ASTC*`, uncompressed always). It answers one question — *is this configuration previewable on this
  GPU?* — and **gates on the feature, not a platform label** (an Intel Mac and an Apple-Silicon Mac
  are both "macOS" but differ in exactly the BC bit that matters).

- **"Preview as ship config" — opt-in WYSIWYG.** A preview selector lets the author eyeball the real
  BC7/ASTC artifacts of a ship configuration. The selector **disables host-incompatible configurations
  with a stated reason** — *"macOS-Mobile (ASTC4x4) — not previewable: this GPU lacks ASTC.
  Build-only."* Choosing a previewable configuration re-cooks the visible textures against its formats
  and remounts behind the stable `AssetHandle`s (the existing recook + hot-reload path, triggered by a
  configuration change instead of a source edit). The default remains host-safe; this is the opt-in.

- **A never-stuck fallback.** If **no** configuration is host-previewable (a mobile-only project opened
  on a BC-less GPU), the editor previews host-safe with a banner — *"previewing uncompressed; no build
  configuration targets this GPU"* — and stays fully editable. Authoring is never blocked by the host's
  sampling limits.

- **The capability warning reuses the gate.** The same `IsAstcSupported()`-style check that gates
  preview eligibility also drives an "active configuration not supported on this GPU" notice in the
  Project Settings panel — one query, two uses (the design doc's stated economy).

## Decisions

1. **Building is unrestricted; previewing is gated.** The CPU encoder cooks any configuration on any
   host (shipping the mobile pack from a Windows box is normal). Only the editor's *sample* path is
   bounded by host capability.
2. **The default preview is host-derived, so the bad path cannot occur.** The live-cook default is a
   host-safe profile chosen from the device queries, independent of the selected ship configuration —
   the failure is structurally impossible, not merely warned against.
3. **Gate on device features, not platform strings.** `IsBlockCompressionSupported()` /
   `IsAstcSupported()` (planset-33's enabled-state queries) decide eligibility; "macOS" vs "Windows"
   never does.
4. **"Preview as ship config" is opt-in and disables the impossible.** The selector greys out
   host-incompatible configurations with a reason; the author opts in to WYSIWYG, the editor never
   silently fails.
5. **The editor is never stuck.** No previewable configuration → host-safe preview + a banner, fully
   editable. Authoring a target the current GPU cannot sample is a normal, supported workflow.

## Files

| File | Change |
|---|---|
| `editor/src/EditorHost.cpp` | Choose the host-safe live-cook default from the device queries; intersect a configuration's formats with host capability; the never-stuck fallback. |
| `editor/src/Panels/ProjectSettingsPanel.cpp` | The "preview as ship config" selector disabling host-incompatible configurations with a reason; the active-config capability warning. |
| `editor/src/Panels/TextureEditorPanel.cpp` | Preview through the host-clamped configuration; re-cook + remount on a preview-config change; the fallback banner. |
| `editor/src/…` (a capability helper) | `IsConfigPreviewable(const BuildConfiguration&, const Context&)` over the planset-33 queries. |

## Verification

- Clean build; `ctest` green — editor targets build; editor smoke paths unaffected.
- Manual editor check (recorded in notes): on the ASTC-capable dev Mac, the macOS configuration is
  previewable and the Windows/BC7 configuration's preview-eligibility reflects the host's BC support;
  selecting an unsupported configuration greys out with the stated reason and the default preview stays
  host-safe.
- `validation_gate` green — the editor never creates an image in a format the device lacks the enabled
  feature for (the whole point of the gate); no new validation error.
- `smoke_golden` does **not** move; `include_hygiene` unaffected (editor-internal).

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
