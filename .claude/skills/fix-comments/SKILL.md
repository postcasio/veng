---
name: fix-comments
description: Audit and fix code comments in veng against the house rules — strip plan citations / future-work / historical narrative / decorative version tags / call-site re-documentation, and bring declaration doc comments up to complete Doxygen coverage. Use when the user asks to clean up comments, do a comment pass, doxygen-ify a header, or fix the comments in a file/dir.
---

# fix-comments

Bring a target's comments in line with the **Comments** section of `CLAUDE.md`
(the source of truth — read it first). Two jobs in one pass:

1. **Remove bad comments** — plan citations, future-work, historical narrative,
   decorative version tags, and call-site re-documentation.
2. **Doc-comment coverage** — every declaration in a public header carries a
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

5. **Doc-coverage pass.** For each declaration in a public header missing a doc
   comment, write one in house Doxygen style. For each *existing* declaration
   comment not yet in Doxygen form, convert it (`@brief` first line + detailed body)
   without losing any factual content.

6. **Report** the findings grouped by category, `file:line` each, with the rewrite.

7. **Apply** the edits.

8. **Verify** — `cmake --build build -j 2` (cap at `-j 2`). Comments don't change
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

### Keep (do NOT over-correct — these look like hits but are correct)

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
one `model: sonnet` subagent per batch, and each agent does **both jobs**
(reject-cleanup *and* Doxygen coverage) on its own files. Disjoint file sets cannot
conflict. Do **not** split by job type (a "reject" agent and a "Doxygen" agent over
the same files race on writes).

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

Structure: main seeds + partitions by module → fan out sonnet batches (both jobs
each) → main runs the single build + the cross-file dedup → main synthesizes the
report.

## What this skill does not do

It does not set up the Doxygen tool or a Doxyfile (that is a separate task). It only
ensures the *comments* are in the right format and coverage so a future doc-gen run
is complete. If the user wants enforced coverage, suggest a Doxygen run with
`WARN_IF_UNDOCUMENTED` + warnings-as-errors as a follow-on.
