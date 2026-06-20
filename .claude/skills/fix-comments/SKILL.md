---
name: fix-comments
description: Audit and fix code comments in veng against the house rules — strip plan citations / future-work / historical narrative / decorative version tags / call-site re-documentation, tighten verbose comments down to their essential why, and bring declaration doc comments up to complete Doxygen coverage. Use when the user asks to clean up comments, do a comment pass, doxygen-ify a header, or fix the comments in a file/dir.
---

# fix-comments

Bring a target's comments in line with the **Comments** section of `CLAUDE.md`
(the source of truth — read it first). Three jobs in one pass:

1. **Remove bad comments** — plan citations, future-work, historical narrative,
   decorative version tags, and call-site re-documentation.
2. **Tighten the survivors** — a comment that earns its place is still wrong if it
   takes a paragraph to say one thing. Cut every kept comment down to the shortest
   form that still carries its *why*. See **Concision** below — this is the job most
   easily skipped, and the one the user cares about most.
3. **Doc-comment coverage** — every declaration in a public header carries a
   Doxygen `///` doc comment, complete enough that a doc generator produces a full
   reference. Inline body comments stay plain `//` and follow the same content
   rules.

This skill **reports, then auto-applies**: it prints the categorized findings, makes
the edits, and builds. The user reviews the resulting diff.

## Scope — a target is required

There is no default scope. The invocation must name what to operate on:

- a **file** (`engine/include/Veng/Renderer/Image.h`),
- a **directory** (`engine/include/Veng/Renderer/`),
- `diff` — the git-changed / uncommitted files,
- `all` — the full sweep (`engine assetpack cooker examples editor`).

If no target was given, ask for one before doing anything. Do not assume `all`.

## Procedure

1. **Read `CLAUDE.md`'s Comments section.** It is the rubric; this file restates it
   operationally but `CLAUDE.md` wins on any conflict.

2. **Resolve the target** to a concrete file list (`git diff --name-only` for
   `diff`; the five top dirs for `all`).

3. **Seed candidates with grep.** Fast, wide net — most hits are *false positives*,
   so this only finds lines to *read*, never to blindly edit:

   ```sh
   grep -rniE '//.*(plan[ -]?[0-9]|planset|for now|future work|not yet|we will|\bv1\b|\bv2\b|used to|previously|formerly|no longer|ported from|extracted from|TODO|FIXME|HACK)' <files>
   ```

   Separately, list every **declaration** in scope (public headers especially) to
   check doc-comment coverage — a class/struct, public method, public field, enum +
   enumerators, free function, macro, public type alias.

4. **Classify each candidate in context** against the rubric below. Read the
   surrounding lines; do not judge from the grep line alone. The keep-list is what
   makes this safe — apply it before deleting anything.

5. **Concision pass — read *every* comment, not just grep hits.** The verbosity job
   has no keyword to grep for, so the only way to find a bloated comment is to read
   the whole comment. Any block of 3+ comment lines is a candidate; rewrite it to the
   shortest form that keeps its *why* (see **Concision**).

6. **Doc-coverage pass.** For each declaration in a public header missing a doc
   comment, write one in house Doxygen style. For each *existing* declaration
   comment not yet in Doxygen form, convert it (`@brief` first line + detailed body)
   without losing any factual content.

7. **Report** the findings grouped by category, `file:line` each, with the rewrite.

8. **Apply** the edits.

9. **Verify** — `cmake --build build -j 2` (cap at `-j 2`). Comments don't change
   logic, but this catches an edit that ate real code or broke a `/* */` block. A
   green build is the gate before you call it done.

## The rubric

### Reject (fix or delete)

1. **Plan / planset citations** — `(plan 07)`, `planset-3`, "see plans/…". Strip
   the reference; keep any factual remainder.
2. **Future-work / temporariness** — "for now", "later we will", "future work",
   "not yet supported". State a real limitation as a present-tense fact, or cut it.
3. **Decorative version tags** — `v1`/`v2` in prose meaning "a later version
   differs". Drop the tag; describe what the code *is*.
4. **Historical narrative** — "used to", "previously" / "no longer" / "formerly"
   *contrasting with old source*, "ported from", "extracted from". Restate in
   present tense.
5. **Call-site re-documentation** — a comment at a usage site that describes the
   *callee's* general behavior instead of *why this call is here*. Replace with the
   local intent, or delete if self-evident. If one contract recurs at many call
   sites, document it once and reference it at the rest.
6. **Project-specific jargon where a standard term exists** — prefer the
   industry-standard word a graphics/systems engineer would already know over an
   internal coinage. Rewrite veng shorthand into the plain concept: "retire the
   handle" → "defer destruction until the GPU is done"; "the relocatable trio" →
   "the launcher, module, and asset pack"; "the recompile seam" → "the point where
   the pipeline set is rebuilt". The goal is prose a newcomer reads without a
   veng glossary.

### Keep (do NOT over-correct — these look like hits but are correct)

- **An actual identifier or type name** — `SceneRenderer`, `BindlessRegistry`,
  `AssetId`, `Context::AcquireNextFrame`. Naming the real symbol is not jargon; keep
  it spelled exactly. The jargon rule targets *descriptive prose*, not the names of
  things the code defines.
- **Industry-standard terms that only look project-specific** — "bindless",
  "g-buffer", "deferred lighting", "frustum culling", "broadphase", "BVH", "PSSM
  cascades", "tonemap", "frame-in-flight" are standard graphics vocabulary. Keep
  them; do not "simplify" a precise term into a vaguer one.
- When a veng concept has no standard equivalent, keep its name but anchor it to the
  nearest standard idea on first use, rather than assuming the reader knows it.

- **`previously` / `later` about program execution**, not source history — "clear
  any previously bound pipeline", "a later graph pass declares the same use", "lands
  on a later frame". Factual. Keep.
- **A real, checked format version** — a version number the code validates and
  rejects on mismatch (e.g. `CookedBlobs`/`CookedMaterialVersion`). Keep; describe
  it as the fact it is.
- **Load-bearing `why` comments** — the MoltenVK argument-buffer rationale, the
  frames-in-flight safety argument, "set 0 is reserved for bindless". CLAUDE.md
  explicitly wants these; length is not a defect. Keep.
- **Genuine local intent at a call site** — "build at runtime rather than load a
  cooked mesh, so the tessellation shows UV mapping without pole clustering". That
  explains a *choice this code made*. Keep.

The deciding test (from CLAUDE.md): *would this sentence still be true and useful to
someone who never saw the roadmap or the git history?* If its only job is to anchor
the code to its past or future, cut it.

## Concision

A correct comment is still wrong if it's three times longer than it needs to be. The
reject rules above remove *bad* comments; this rule shrinks the *good* ones. After a
pass, the comments should be noticeably shorter, not merely cleaner.

**The default length is one line. Use two only when there's a genuine non-obvious
*why* that doesn't fit.** Three or more lines of inline comment is a smell — it almost
always means the comment is doing one of the three things below. (Doc comments on a
public declaration are exempt: a full `@brief` + contract is the goal there.)

Cut a sentence when it:

- **(a) describes what the callee does** rather than why *this* call is here. The
  callee's behavior is documented on the callee. (This overlaps reject category 5 —
  concision is where you notice the bloated ones.)
- **(b) restates a contract documented elsewhere** — an invalidation rule, an
  ownership rule, a `@pre` already on the declaration. Reference it in three words
  (`see ReconfigureScene`) instead of re-deriving it.
- **(c) inventories structure the code already shows** — "X owns A, Y owns B, Z owns
  C" when the next five lines *are* `a.reset(); b.reset(); c.reset();`. State the one
  reason the block exists, not a line-item map of it.

Keep the load-bearing `why`; drop the tour. Worked examples (all real, from
`examples/hello-triangle/main.cpp`):

```cpp
// ── 6 lines → 1.  (a): the SceneRenderer's job is documented on SceneRenderer.
// One SceneRenderer drives the main view; the sample composites its output, the smoke path downloads it.

// ── 6 lines → 1.  (a): Mount/LoadSync's dependency-loading belongs on those decls.
// Mount from the executable's directory so the pack resolves wherever the launcher is copied.

// ── 5 lines → 2.  Keep the genuine why (UV mapping), drop the rest.
// Built at runtime, not cooked: an icosphere's near-uniform tessellation shows the
// brick UV mapping without a UV sphere's pole clustering.

// ── 4 lines → 2.  Two reasons survive: reproducible pose, and bloom headroom.
// Fixed direction so the smoke pose is lit reproducibly; intensity pushes facets
// past 1.0 in linear HDR to give bloom something to act on.

// ── 6 lines → 2.  (b): the recreate-output/re-bind contract lives on ReconfigureScene.
// Entries mirror the DebugView enum in declaration order, so the combo index is the
// enum value. Selecting one recompiles via ReconfigureScene.

// ── 6 lines → 1.  (c): the comment inventoried five owners the reset() calls already show.
// Release every engine resource before the context tears down (see Application::OnDispose).
```

## Doxygen house style

Full spec lives in `CLAUDE.md`. In brief:

- `///` line comments, `@`-prefixed tags. No `/** … */`, no `\brief`.
- First line `@brief` (one sentence) → blank `///` → detailed body (existing
  rationale prose moves here unchanged in spirit, still bound by the reject rules).
- Contract tags as warranted: `@param`, `@tparam`, `@return`, `@pre`/`@post`,
  `@warning`, `@see`. Don't pad a self-evident one-arg setter with boilerplate.
- Short field/enumerator docs may be trailing `///<`.
- **Coverage:** every declaration in a public header is documented; internal
  (`*/src/`) headers document every non-trivial declaration.

## Delegation

Parallelize only for `all` or a large directory. For `diff`, a single file, or a
handful, do it inline — a subagent is not worth the spin-up.

The only parallel axis is **by file**: partition the target into disjoint batches,
one `model: sonnet` subagent per batch, and each agent does **all three jobs**
(reject-cleanup, concision, *and* Doxygen coverage) on its own files. Disjoint file
sets cannot conflict. Do **not** split by job type (a "reject" agent and a "Doxygen"
agent over the same files race on writes).

Partition rules:

- **By module, not blindly by directory.** Keep a public header and its paired
  implementation in the same batch (`Image.h` + `Backend/Image.cpp` together) so one
  agent sees both tiers and files each fact correctly — API contract on the header's
  Doxygen comment, local `why` in the `.cpp`'s inline comments.
- Balance batches by file count/size; aim for 4–6 for a full sweep.
- Hand each agent the full rubric and Doxygen style from this file (so they converge
  on one style) and its exact file list.

Stays on the main thread — never delegated:

1. **Seed grep + partition** — once, up front.
2. **The build** — one shared `build/`; run `cmake --build build -j 2` a single time
   after all batches return. Concurrent builds of one dir are illegal.
3. **Cross-file call-site dedup (category 5)** — "one engine contract recurs at many
   call sites → document once, reference at the rest" needs a global view a per-file
   agent lacks. Do this coordinating pass over call sites after the batches land.
4. **The synthesized report.**

Structure: main seeds + partitions by module → fan out sonnet batches (all three
jobs each) → main runs the single build + the cross-file dedup → main synthesizes the
report.

## What this skill does not do

It does not set up the Doxygen tool or a Doxyfile (that is a separate task). It only
ensures the *comments* are in the right format and coverage so a future doc-gen run
is complete. If the user wants enforced coverage, suggest a Doxygen run with
`WARN_IF_UNDOCUMENTED` + warnings-as-errors as a follow-on.
