# Plan 01 — ring-buffered material parameter writes

**Goal:** make a per-frame `Material::SetParam` / `SetTexture` cheap and frame-safe. Today a
material's block lives in a single device-local SSBO written by `UploadSync` (a staging copy
behind a `WaitIdle`) — fine for a write-once cooked material, a full GPU stall and a
frames-in-flight hazard if done every frame. This plan N-buffers the unified material block
buffer (plan 00) so the per-frame writers the PostProcess path needs — the runtime-bound
input handle (plan 04) and the live `Exposure` param (plan 05) — write a host-mapped region
directly, with no stall and no hazard. **Authored params stay buffer-backed** — there is no
push-constant side channel; the buffer is simply made safe to write per frame.

## The hazard, precisely

`BindlessRegistry::UpdateMaterial` calls `m_MaterialParamBuffer->UploadSync(...)`
([BindlessRegistry.cpp:198](../../engine/src/Renderer/Backend/BindlessRegistry.cpp)) — a host
memcpy into a staging buffer plus a queue copy under `WaitIdle`. The material buffer is
**single-copy**: one region per material slot, shared across every frame-in-flight. Writing a
slot's bytes on the CPU while a prior frame's GPU work may still be reading that slot is a
data race; `UploadSync`'s `WaitIdle` masks it only by stalling the whole device every write.
At load that cost is paid once and is invisible. Per frame it is a stall per material per
frame — unacceptable, and the reason plan 05's "set exposure each frame" cannot use the
current path as-is.

## What lands

### The material buffer becomes N-buffered and host-mapped

The single material block buffer (plan 00, set 0 binding 4) is allocated **`framesInFlight`
copies** wide — `framesInFlight * MaxMaterials * MaterialParamStride` — as a
**host-visible, persistently-mapped** buffer (`HOST_VISIBLE | HOST_COHERENT`; MoltenVK serves
this directly). Each frame-in-flight `f` owns the region `[f * MaxMaterials *
MaterialParamStride, ...)`. A write into the **current** frame's region is always safe (that
frame has not been submitted yet); the other in-flight regions are never touched by a write,
so no fence wait is needed and `UploadSync` is gone from this path.

### The shader selects its region by a dynamic offset

The set-0 material binding becomes a **dynamic storage buffer**
(`STORAGE_BUFFER_DYNAMIC`); `BindlessRegistry::Bind(cmd)` supplies the per-frame dynamic
offset `currentFrameInFlight * MaxMaterials * MaterialParamStride`. The shader's indexing is
**unchanged** — it still reads its block at `idx * MaterialParamStride` within the bound
region; the dynamic offset shifts the window to the current frame's copy. (If the descriptor
abstraction does not yet thread a dynamic offset through `Bind`, adding that is part of this
plan — it is one offset, supplied where set 0 is already bound once per pipeline bind.)

### Writes flush over the in-flight window

A write must reach every frame's region but may only ever touch the *current* region. So the
registry tracks a per-material **dirty counter**: `RegisterMaterial` and `UpdateMaterial` set
it to `framesInFlight`, and `OnFrameAcquired(f)` — already the registry's per-frame hook —
memcpys each still-dirty material's block into region `f` and decrements its counter. A
cooked material (written once) propagates across all N regions over N frames, each write
hitting only the safe current region; a value re-set every frame (live exposure) stays dirty
and lands in each region as that frame comes current. No `WaitIdle`, no staging, no race.

## Decisions

1. **Ring the buffer; keep params in the buffer.** A per-frame knob (exposure) and a
   per-frame input handle are authored material data — they belong in the material's block,
   read through the standard set-0 path, visible to the inspector and the node editor. The
   fix is to make the buffer safe to write per frame (N-buffer + host-map + dynamic offset),
   not to special-case some values into push constants. The material model stays uniform:
   every field, cooked or per-frame, is a block field.

2. **Dirty-flush over the in-flight window, not write-all-N-now.** Writing all N regions at
   the moment of a `SetParam` would stomp regions a prior in-flight frame is still reading —
   the very hazard. Writing only the current region each frame (driven by a dirty counter
   that survives N acquires) is the standard ringed-dynamic-buffer discipline and is the only
   write that needs no fence wait.

3. **Host-visible coherent, persistently mapped.** The buffer is mapped once at creation and
   memcpy'd into directly; no per-write map/unmap, no flush (coherent). The memory cost is
   `framesInFlight * MaxMaterials * MaterialParamStride` ≈ 3 × 256 × 256 = 192 KiB —
   negligible. This trades the device-local + staging path (good for write-once data) for a
   host-mapped path (good for per-frame data); for a 256-material param store the bandwidth is
   trivial and the win is removing the per-write `WaitIdle`.

4. **Single render thread, so no further sync.** Writes happen on the one render thread
   (`OnFrameAcquired` runs in `Context::AcquireNextFrame`, before recording); the current
   region is written before the frame is submitted and read. The existing frames-in-flight
   fence (waited at acquire) is exactly the guarantee that the region being written is the one
   whose prior use has completed. No semaphore, no extra barrier.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/Renderer/BindlessRegistry.h` | Material buffer sized `* framesInFlight`; per-material dirty counter; doc the ring + dynamic-offset model. |
| `engine/src/Renderer/Backend/BindlessRegistry.cpp` | Host-visible persistently-mapped material buffer; `Register/UpdateMaterial` set dirty + memcpy current region; `OnFrameAcquired` flushes dirty regions; `Bind` supplies the dynamic offset; drop `UploadSync` from this path. |
| `engine/src/Renderer/Backend/DescriptorSet.cpp` (+ layout) | Material binding → `STORAGE_BUFFER_DYNAMIC`; thread the dynamic offset through `Bind` if not already supported. |
| `tests/gpu/*` | A test mutating a material's param across consecutive frames and asserting each frame reads its own value (no tearing, no stale read); the `validation_gate` covers the dynamic-offset descriptor usage. |

## Verification

- Clean build; the material buffer is host-mapped and N-buffered; no `UploadSync` on the
  material-param path.
- `gpu` band: a material whose param changes each frame renders the correct per-frame value
  (a write-then-read across the in-flight window is correct); a write-once cooked material is
  stable after the N-frame flush.
- `validation_gate` (under `build-debug`) clean — the dynamic storage buffer is bound with a
  valid offset and range; no out-of-bounds or unaligned dynamic-offset error.
- `smoke_golden` unchanged — brick's params are static and read identically; this plan changes
  *how* the buffer is written, not any value.
