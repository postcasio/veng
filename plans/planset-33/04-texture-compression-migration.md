# Plan 04 — texture-compression migration + golden

**Goal:** migrate `examples/hello-triangle` onto the new pipeline (cooked **mips + ASTC** textures over a
**zstd-compressed** archive), **regenerate the smoke golden** on an ASTC-capable device, and gate it for
devices that lack the codec. Depends on Plans 00–03; the docs pass that closes the planset is Plan 05.

## What lands

- **hello-triangle migrated to ASTC.** Its textures cook with mips + **ASTC** (the cook default from Plan
  03) into a zstd-compressed `.vengpack`; confirm the sample pack shrinks materially on disk and the
  **relocatable trio** (launcher + `libhello_triangle` + pack) still runs from a fresh directory and
  writes a correct-sized PPM on an ASTC-capable device.

- **Smoke golden regenerated, and gated.** ASTC is lossy, so the capture moves. Re-shoot via the
  documented path (`HT_SMOKE=/tmp/ht.ppm … hello_triangle-launcher`, `sips -s format png`) on an
  **ASTC-capable** device, and confirm `smoke_golden` passes within the existing fuzz tolerance —
  widening it only if ASTC artifacts demand it, with the change documented in the test's
  allowlist/threshold and the exact encoder tag + quality preset recorded. Because the migrated scene now
  requires `textureCompressionASTC_LDR` (a device without it gets `AssetError::Unsupported` and renders
  untextured), **`smoke_golden` skips on a non-ASTC device** (the `gpu` `SKIP_RETURN_CODE 77` pattern) so
  the golden is only asserted where it is reproducible. Note in the test that the golden is now
  codec-dependent.

- **What a missing-codec run looks like.** On a device lacking the cooked codec the texture loader logs
  `AssetError::Unsupported` once and the affected materials sample their fallback (untextured) — the app
  still runs and the launcher smoke still exits 0 with a correct-sized PPM; only `smoke_golden` (which
  skips there) would otherwise diverge. (Plan 05 records this behavior in `engine/CLAUDE.md`.)

## Files

| File | Change |
|---|---|
| `examples/hello-triangle/assets/…` | Texture sources / manifest cook to mipped ASTC; confirm the pack builds and shrinks. |
| `tests/golden/hello_triangle_scene.png` | Regenerated capture (ASTC-lossy), on an ASTC-capable device. |
| `tests/…` (`smoke_golden`) | Gate the golden to skip on a non-ASTC device; note codec-dependence + encoder tag/preset. |
| `plans/planset-33/README.md` | Status column → `done` for 00–04. |

## Verification

- Full `ctest` + `smoke_golden` + `validation_gate` green on an **ASTC-capable** device; `smoke_golden`
  **skips** (not fails) on a non-ASTC device.
- `vengc verify` green on the re-cooked sample pack; the `.vengpack` is materially smaller than the
  pre-compression baseline.
- The relocatable trio runs from an unrelated working directory and exits 0 (on an ASTC-capable device).
- `include_hygiene` green.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.
