# userguide — the user guide

A MkDocs Material site documenting how to **use** veng. The hand-written guide
lives in `docs/`; the API reference under it is generated from the public headers'
Doxygen comments by the `mkdoxy` plugin. Build and toolchain details are in
[README.md](README.md). This file is the writing style the guide is held to.

## Audience: users, not contributors

The guide answers one question — *how do I use this engine?* — and nothing else.

Out of scope, always:

- **Engine internals and architecture.** No public/backend split, no Native idiom,
  no "set 0 is the engine's because MoltenVK…", no descriptions of how a subsystem
  is implemented. A reader wants to drive the engine, not maintain it.
- **The build system, tests, linting, CI.** How veng itself is built, formatted,
  tidied, or verified belongs in the repo, not here. (A user *building from source*
  needs the configure/build commands — that much is in scope; the rest is not.)
- **Rationale for how the engine is built.** "X is done this way because…" is
  almost always internal. State what the user does and what happens, not why the
  engine is shaped the way it is.

If you cannot tie a sentence to something the reader will *do* with the engine, cut
it.

## Don't duplicate the API reference

The generated reference already lists every type, method, field, and enum. The
guide must not restate it.

- No page or section that is just an API listing or a type's contract.
- Don't explain standard-library types. `Ref<T>` and `Unique<T>` are
  `std::shared_ptr` and `std::unique_ptr`; name them once in the conventions
  glossary and move on. Nothing about smart-pointer mechanics.
- Write what isn't in a header signature: when to call a thing, how pieces fit
  together, the behaviour you can't read off a declaration (e.g. that dropping a
  resource mid-frame is safe).

## Voice: plain and dry

Technical documentation, not a pitch. Nothing here is selling anything.

Banned — these are the tells to strip on sight:

- Marketing adjectives: *powerful, seamless, robust, elegant, rich, first-class,
  batteries-included, modern* (as praise).
- LLM filler and framing: *load-bearing, the spine, a taste, for free, handy, worth
  knowing, deliberately, simply, just, essentially, under the hood, think of…,
  Most apps live here, Learn this once and…*.
- Rhetorical scaffolding: "It's worth noting that", "Note that", "Keep in mind",
  "The one thing to remember".
- Hype punctuation: exclamation marks; em-dash pile-ups used for drama.

Prefer short declarative sentences. If a plainer word exists, use it. One idea per
sentence.

## Accuracy: verify against the source

Get the API right by reading the code, not by guessing or trusting prose.

- The **canonical example** (`examples/hello-triangle`) and the **public headers**
  are the source of truth for how the engine is used and what signatures look like.
- The repo `README.md` can be **stale** — it has carried out-of-date signatures.
  Don't copy code from it without checking the headers/example.
- Don't invent prescriptive rules or architecture. If you're unsure how something
  is meant to be used, read the source before writing a claim. (This guide has
  shipped wrong "always do X" rules and stale call signatures; both came from not
  checking.)

## Present tense, factual

Same rule as the engine's code comments (see the root `CLAUDE.md`): describe the
engine as it is now. No history, no roadmap, no "for now" / "currently" / "in a
future version".

## Callouts sparingly

An admonition (`!!! warning`, `!!! note`) is for a genuine gotcha a reader will hit
— calling the API off the render thread, a method that invalidates a cached handle.
Most pages need none. Never use one for decoration or emphasis.

## Bias to pruning

When in doubt, cut. A shorter guide that says only true, useful things beats a
thorough one padded with the obvious. Remove a section rather than pad it; remove a
sentence rather than soften it.

## Mechanics

- Hand-written pages are Markdown under `docs/`, listed in the `nav` in
  `mkdocs.yml`.
- The API reference is generated at build time by `mkdoxy` from the public headers
  — there are no checked-in API Markdown files, and the guide links into it rather
  than reproducing it.
- Build with `mkdocs build --strict`; a broken internal link fails the build, so a
  clean strict build is the bar before committing.
