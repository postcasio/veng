# Plan 05 — docs + roadmap re-cut

**Goal:** document the landed `Veng::UI` toolkit and re-cut the roadmap. No code.

**Depends on** plans 00–04 (it documents what they delivered).

## Changes

1. **`CLAUDE.md` — add a `Veng::UI` core-conventions paragraph.** A new subsection under
   **Core conventions** (near the ImGui/Editor material), present-tense, stating the toolkit
   as a fact:
   - `Veng::UI` (`engine/include/Veng/UI/`, in `libveng`) is the engine-tier immediate-mode
     vocabulary fronting ImGui; UI is authored against it, not raw `ImGui::`, at every
     widget site (game modules and the editor both).
   - One `Drag` overloaded on `f32`/`vec2`/`vec3`/`vec4`/`i32`; options are
     designated-initializer structs (`DragOptions`, `SliderOptions`), never `ImGui*Flags`;
     edits return `[[nodiscard]] bool`; text is preformatted `string_view` via `fmt`.
     RAII scope guards (`UI::Window`/`TreeNode`/`Table`/`Menu`/`PushId`/`StyleVar`/…) replace
     every begin/end and push/pop pair, closing on scope exit.
   - The `Veng/UI/` headers are imgui-free in their signatures; `<imgui.h>` appears only in
     `engine/src/UI/`. ImGui stays a **PUBLIC** dependency (wrapper-only) — driving it
     PRIVATE is a possible later planset `Veng::UI` unblocks.
   - The boundary: ImGui **frame lifecycle and host/dock/present plumbing** stay raw in
     `ImGuiLayer`/`EditorHost`; keyboard/mouse queries stay thin and converge with the
     event/input area. `Veng::UI` replaces `ImGui::` only at authoring sites.
   - `EditorPanel::GetWindowFlags()` returns `UI::WindowFlags`.

2. **`plans/README.md` — add the planset-17 index entry.** A paragraph in the planset list
   (after planset-16), summarizing: takes up future area 12 (UI toolkit); the engine-tier
   `Veng::UI` base vocab (overloaded `Drag`, options structs, RAII scopes, imgui-free
   signatures) + full migration of every widget-authoring `ImGui::` site (hello-triangle's
   game-module debug panel to zero raw `ImGui::`, the reflection inspector's `FieldClass`
   dispatch through one `Drag`, the editor panels + menu bar); wrapper-only (ImGui stays
   PUBLIC); frame-lifecycle/dock/present plumbing + thin key/mouse queries stay raw; the
   stateful editor-widget-class pattern deferred.

3. **`plans/future/README.md` — mark area 12 done.** Update the area-12 section, the
   "Ordering & dependencies" bullet, and the **Status** block: area 12 (UI toolkit —
   `Veng::UI`) moves out of "Undetailed / unscheduled" into delivered-by-planset-17, with
   the "drive imgui private" end-state and the stateful-widget-class follow-on noted as the
   remaining UI-tier future work. (Area 11 `ImGuiCompositePass` is already delivered by
   planset-16 — leave that as-is.)

4. **`plans/planset-17/README.md` — flip the status column** entries to `done` as each
   plan lands, and confirm the **On completion** section reads true.

## Files

| File | Change |
|---|---|
| `CLAUDE.md` | New `Veng::UI` core-conventions paragraph. |
| `plans/README.md` | planset-17 index entry. |
| `plans/future/README.md` | Area 12 → delivered; status block re-cut. |
| `plans/planset-17/README.md` | Status column → `done`. |

## Verification

- Docs-only; no build/test impact. Confirm the `CLAUDE.md` paragraph follows the
  comment/prose policy (present-tense facts, no "used to call ImGui directly" narrative) and
  that the `plans/README.md` entry matches the established per-planset summary cadence.
- A final `grep -rn "ImGui::" engine examples editor` sanity sweep confirms the only
  surviving raw `ImGui::` is the documented integration-layer boundary
  (`ImGuiLayer`/`EditorHost` lifecycle + dock/present) and the decision-5 flagged
  key/mouse-query sites — matching what the new `CLAUDE.md` paragraph claims.
