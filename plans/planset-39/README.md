# planset-39 — grab bag: material instances, codecs, emissive, atmosphere, allocation honesty

**Phase goal:** a coherent grab bag, anchored by one substantial feature (a precomputed atmospheric
sky and the dynamic ambient it feeds) and rounded out with material/codec/renderer wins and one
deliberate revert. The threads are independent; they ride together because each is small enough not to
warrant its own planset — except the atmosphere, which is here because it is worth doing, not because
it is easy. Four threads:

1. **Material instances finish their story.** Retire planset-38 Plan 05's parent-id **overload** (one
   `AssetId` naming both a `Material` and its implicit default `MaterialInstance`) for explicit
   default-instance ids, then make the editor mint those ids so the hand-mint is automatic.
2. **Texture codecs specialize per channel.** `Normal → BC5`, `Mask → BC4` on the role→format table,
   with the ASTC normal-packing convention.
3. **Atmosphere + dynamic ambient.** A Bruneton precomputed sky (the first real `Type3D` consumer),
   an SH math primitive, and a dynamic SH ambient that lets the sky light the scene and move with the
   sun — plus color-decoupled emissive via an additive forward pass.
4. **Allocation honesty.** Remove planset-32's allocation-tier outer loop, which hitches; keep the
   sub-rect inner loop.

## Thread 1 — explicit, auto-minted material-instance ids

planset-38 Plan 05 retyped every material reference from `AssetHandle<Material>` to
`AssetHandle<MaterialInstance>` but left the cooked id values pointing at the parent `Material` ids, so
a parent-material id was made *also* serviceable as a `MaterialInstance` request — via a composite
`(type, id)` cache key and an `AssetManager::Resolve` default-instance bridge. That bought zero
migration churn at the cost of three standing compromises: one id naming two assets (breaking the "one
id ⇒ one asset" invariant the cache, the prefab rehydrate path, and tooling assume); a type-mismatch
branch in the hot resolve path; and an id-less parent handle synthesized at load.

**Plan 01** takes the explicit-id path: a parent `*.vmat.json` declares a minted **`defaultInstance`**
id, the cook emits a real zero-override `MaterialInstance` at it, every reference (in every pack and
every C++ literal) is rewritten to name it, and the overload — the composite cache key, the resolve
bridge, `LoadDefaultInstance` — is deleted. **Plan 02** then makes the editor mint and write the
`defaultInstance` id on material create/save, so the hand-mint Plan 01 requires becomes automatic for
editor-authored materials (cook-time minting is rejected — it would mutate source during the build or
invent an unreferenceable id).

## Thread 3 — the atmosphere anchor

Emissive that is independent of albedo, a sky that is physically based and dynamic, and the indirect
lighting that sky produces. **Plan 04** adds color-decoupled emissive as an additive forward pass into
the lit HDR target (no fourth g-buffer target — emissive is an additive output, not a lighting input).
**Plan 05** lands the SH math (`Veng/Math/SphericalHarmonics.h`) foundation-first, fully unit-tested,
with no consumer of its own. **Plan 06** is the heavy anchor: Bruneton precomputed atmospheric
scattering — LUTs precomputed once (the 4D scattering table packed into a **`Type3D`** texture, the
first use of veng's volume-texture capability), the sky a cheap runtime LUT sample for any sun
direction. **Plan 07** projects that sky into SH each frame and wires it as the third ambient arm
(`IBL : skylightSH : flat constant`), so the no-environment ambient becomes a directional sky fill
that tracks the sun.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 01 | [Explicit default-instance ids; drop the `(type,id)` overload](01-explicit-default-instance-ids.md) | A parent `*.vmat.json` declares a minted `defaultInstance` id; the cook emits a companion zero-override `MaterialInstance` at it; every material reference (packs + C++ literals) is rewritten to it. The composite `(type,id)` cache key reverts to id-only and the resolve bridge + `LoadDefaultInstance` are deleted. `smoke_golden` holds. | proposed |
| 02 | [Editor generates the `defaultInstance` id](02-editor-default-instance-id.md) | The material editor mints + writes the `defaultInstance` id on create/save via an id generator on the injected `CookBackend`, so editor-authored materials never need the hand-mint. Cook-time minting is rejected (source mutation / invented id). | proposed |
| 03 | [BC5/BC4 channel specialization](03-bc5-bc4-channel-specialization.md) | `Normal → BC5` (two-channel), `Mask → BC4` (single-channel) on the role→format table; the ASTC normal-packing (XY + Z reconstruct) convention; one shared codec-agnostic normal-unpack shader helper. | proposed |
| 04 | [Additive forward emissive](04-additive-forward-emissive.md) | Color-decoupled emissive via an additive forward `EmissiveScenePass` into the lit HDR target (no fourth g-buffer target). RGB emissive material term, a `DebugView::Emissive` arm, a settings toggle. | proposed |
| 05 | [`SphericalHarmonics.h` — the SH math](05-spherical-harmonics-math.md) | A pure, device-free order-2 SH primitive in `Veng/Math/` — project / cosine-convolve (radiance→irradiance) / evaluate — fully unit-tested, no consumer in this plan. | proposed |
| 06 | [Bruneton atmospheric sky](06-bruneton-atmospheric-sky.md) | Precomputed atmospheric scattering: transmittance/scattering/irradiance LUTs (the 4D scattering table as a `Type3D` texture — the first volume-texture consumer), a runtime sky pass, an `Atmosphere` settings struct. Aerial perspective + sky-driven specular prefilter are named follow-ons. | proposed |
| 07 | [Dynamic SH ambient](07-dynamic-sh-ambient.md) | Project the Plan 06 sky into SH each frame and wire it as the third ambient arm (`IBL : skylightSH : flat constant`), so the no-environment ambient is directional and tracks the sun. Depends on 05 + 06. | proposed |
| 08 | [Remove the allocation-tier outer loop](08-remove-allocation-tier.md) | Delete planset-32's `StepAllocationTier` + the tier-driven `Resize` (it hitches); keep the per-frame sub-rect inner loop over a fixed allocation. Update the roadmap. Supersedes planset-32's outer loop. | proposed |

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = implemented, migrated, verified, committed.

## Dependencies

- **Depends on planset-38 being complete** — Plans 01 + 02 build on Plan 05's `MaterialInstance`
  runtime / the overload they remove and Plan 06's `MaterialInstanceImporter`. Threads 2–4 are
  independent of planset-38.
- **Within this planset:** **01** stands alone; **02** depends on **01** (the `defaultInstance` key it
  fills). **07** depends on **05** (the SH math) and **06** (the sky to project). **03**, **04**,
  **05**, **06**, **08** are mutually independent. **06** is the heaviest; **08** is the smallest.
- Dependent plans must build on the prior plan's integration commit, not `origin/main` — per
  [[project_megaexec_worktree_base]], dispatch **02** against a worktree cut from **01**, and **07**
  against the 05→06 chain; the independent plans can use `isolation: "worktree"` directly.

## The decisions this planset settles

- **One `AssetId` names one asset of one type.** The parent-id overload is removed; a material
  reference names a real, distinct `MaterialInstance` id, and the cache key + resolve path are
  type-agnostic again (Plan 01). The default-instance id is generated at **authoring** time, not cook
  time (Plan 02), keeping the cook a pure source→binary function.
- **Channel intent drives the codec.** A `Normal` is two channels and a `Mask` is one; the role→format
  table resolves them to BC5/BC4 rather than full-channel BC7/ASTC (Plan 03).
- **Emissive is an additive output, not a g-buffer input.** Color-decoupled emissive is a forward
  additive pass; non-emitters cost nothing and no fourth g-buffer target is added (Plan 04).
- **SH is the diffuse-irradiance representation.** It lands as a tested math primitive (Plan 05) with a
  real consumer — a dynamic sky's ambient (Plan 07) — not on spec; the same math serves future
  per-probe GI.
- **The sky is physically based and dynamic.** A precomputed atmosphere (Plan 06) gives a day/night sky
  at runtime-LUT cost and is the first thing to use `Type3D`.
- **Dynamic resolution adapts cost, not allocation.** The hitching allocation-tier outer loop is
  removed; the non-hitching sub-rect inner loop over a fixed allocation stays (Plan 08).

## What remains future

- **Cook-time dead-asset pruning (asset tree-shaking).** A material referenced only as a parent (never
  directly) needs no companion default instance — but that is one symptom of a general missing
  capability: a reachability sweep that drops *any* unreferenced cooked asset from a pack. Filed as its
  own [future area](../future/README.md) (the cross-pack-visibility / exported-root design is the
  interesting part); the default-instance case falls out of it.
- **Aerial perspective + sky-driven specular IBL prefilter** (Plan 06's named follow-ons) — atmospheric
  fog integrated over scene depth, and an amortized prefiltered specular cubemap from the atmosphere for
  dynamic sky reflections.
- **Memory-driven fixed-allocation choice** (Plan 08's named replacement for the removed memory-tier) —
  pick the one fixed allocation up front from a device memory-budget query, distinct from the removed
  perf-driven outer loop.
