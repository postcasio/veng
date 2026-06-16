---
name: nextplan
description: Evaluate the veng roadmap and recommend the best area to tackle for the next planset. Use when the user asks "what's next", which planset to do next, or invokes /nextplan. Surveys plans/future, weighs the open options, and recommends one with a detailed overview plus a couple of lighter alternates.
---

# nextplan

Recommend the **next planset** to build for veng. The roadmap lives in `plans/`;
this skill reads it, weighs every still-open option, and gives the user a clear,
opinionated recommendation — one strong pick in depth, a couple of alternates in
brief.

This is an **advisory** skill. It produces a recommendation in chat. It does **not**
create plan files, scaffold a planset, or write to `plans/`. Stop at the writeup.

## Procedure

1. **Read the roadmap state.** In order:
   - `plans/README.md` — the planset index. Tells you what shipped (planset-1 …
     the latest) and the one-paragraph recap of each. The trailing `future` entry
     names what's still open and what's been superseded.
   - `plans/future/README.md` — the holding area. Areas are numbered for stable
     cross-reference; **DONE** areas carry only a recap, so the live candidates are
     the areas still marked open (historically the editor application, the scene
     renderer, and the event/input systems) plus any **still-future remainders** of
     partly-done areas (e.g. a systems framework, hot-reload's re-cook tail) and the
     **cross-cutting concerns** section.
   - The per-area design docs under `plans/future/` (`editor.md`,
     `scene-renderer.md`, `game-module.md`, `threading-task-system.md`,
     `asset-system.md`, `bindless-descriptors.md`, `compiled-rendergraph.md`) — read
     the ones for your live candidates so the overview is grounded, not guessed.

2. **Establish what's actually in flight.** Check `git log --oneline -15` and
   `git status`. A half-landed planset (uncommitted work, a `planset-NN` dir newer
   than the others, recent `Plan NN:` commits) usually *is* the answer — finishing
   in-flight work beats opening a new front. Note it if so.

3. **Evaluate every open option against these axes:**
   - **Unblocked** — are its prerequisites delivered? The roadmap is explicit about
     dependency edges (e.g. the editor needed the game-module build model and module
     reflection first). Prefer options whose prerequisites are all done.
   - **Prioritized** — the roadmap often names a "prioritized next planset". Respect
     a stated priority unless something has changed it.
   - **Leverage** — does it unblock or de-risk later work, or is it a leaf? A
     "demanding second consumer" that forces the rest of the engine to firm up is
     high-leverage.
   - **Scope coherence** — can it be a single coherent planset (a few plans), or is
     it several plansets that need a slicing decision first?
   - **User signal** — anything the user said this session about where they want to
     go outranks the roadmap's default ordering.

4. **Pick one.** Don't hedge into a tie. If two are genuinely close, lead with the
   one that's unblocked-and-prioritized and mention the other as the top alternate.

## What to produce

Lead with the recommendation, then justify it. Structure:

- **The pick** — one line naming the area and why it's next (unblocked + prioritized
  + leverage, in a sentence).
- **What it involves** — the concrete scope: the major pieces, the new types/systems,
  what existing code it touches, what slicing it implies across plans. Pull specifics
  from the design doc; don't stay abstract.
- **What the result will be** — the end state from the user's seat: what veng can do
  after this planset that it can't now, and what it visibly unblocks next.
- **A usage example, if instructive.** Only when it sharpens the picture — e.g. a
  short sketch of the new API surface, a snippet of how an app/sample would use the
  delivered capability, or a before/after of the sample app. Use veng house-style
  vocabulary (`string`, `Ref<T>`, `vec3`, `Result<T>`, designated-initializer `Info`
  structs). Skip it if the value is purely internal plumbing where a snippet adds
  nothing.
- **Alternates** — one or two other suitable options, each **two or three sentences**:
  what it is, why it's viable, and the one reason it loses to the pick (blocked,
  lower priority, can wait, narrower payoff). Don't expand these into full overviews.

## Style

Match the roadmap's voice: precise, decided, no future-tense hedging in the writeup
itself. Cite plan files as clickable links (`[scene-renderer.md](plans/future/scene-renderer.md)`)
so the user can jump in. Recommend; don't survey. End by offering to go deeper on the
pick or to start scaffolding the planset if the user wants it — but don't scaffold
unprompted.
