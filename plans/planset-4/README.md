# planset-4 — de-globalize the context, then finish testing

**Phase goal:** remove the `Context::Instance()` singleton — thread an explicit
device/context into every resource — and *then*, on top of the explicit-device
API, deliver the testing work that was deliberately held until de-global landed:
the in-process multi-case GPU integration suite (area 5b) and a CI pipeline with a
software Vulkan ICD.

This is **future area 3 (de-globalize the rendering context)** followed by the
**remainder of area 5 (5b + CI)**, in the order the future roadmap fixes:
`5a → 3 → 5b`. planset-3 delivered 5a (the harness + pure-logic/round-trip/
death/one-exe-GPU bands). The de-global change must come before 5b so the GPU
integration suite is written **once** against the explicit-device API and can get
real per-case isolation (stand a `Context` up/down per test) — impossible while
`Context::Instance()` is a process-global singleton.

## Why these two areas in one planset

They are a single arc: de-global is the prerequisite, and 5b is the first real
consumer that *needs* it (per-case device lifecycle). Landing them together means
5b is designed against the final API the moment that API exists, and CI lands with
a suite worth running. The de-global plans (01–04) come first and stand on their
own; the testing plans (05–06) build directly on plan 04's explicit-device API.

## Scope decision

- **In (de-global):** thread an explicit `Context&` into every `X::Create`
  factory; give each resource a back-reference to its context for
  deferred-destruction `Retire` and member ops; convert the context-internal
  primitives off the global; delete `s_Instance` / `Instance()`; update the
  `Veng.h` threading note and the ownership/CLAUDE docs.
- **In (testing):** the in-process multi-case GPU integration suite (5b) with
  per-case `Context` fixtures, and a CI job running `ctest` against a software ICD
  with a validation-error gate.
- **Out of scope — the threading/task system (area 2).** De-global is the
  *prerequisite* for threading, not threading itself. **veng stays single-threaded
  and single-context through all of planset-4.** No concurrency, no second device,
  no transfer queue — those belong to a later threading planset. We remove the
  global; we do not start using the freedom it buys.
- **Out of scope — the descriptor/bindless rework.** The known descriptor-pool /
  `UPDATE_AFTER_BIND` validation gap (CLAUDE.md) is pinned by planset-3's tests and
  belongs to [bindless-descriptors](../future/bindless-descriptors.md), not here.
  5b and CI must not *widen* it; they may continue to note/xfail it.

## Design decisions

### 1. Explicit threading, not a scoped "current device"

The future note left the choice open ("thread an explicit device/context into
creation, **or** a scoped 'current device'"). We choose **explicit threading**:
`X::Create(Context&, const XInfo&)`, and each resource stores the `Context&` it was
created with.

A thread-local "current context" is just the global with extra steps — it keeps the
dependency hidden, the exact smell de-global exists to remove, and it does not
honestly support the multi-device / multi-thread futures this unblocks. Explicit
threading matches veng's house style end to end: the Native idiom and the
vocabulary enums both exist to make the backend dependency **loud**, not implicit.
The cost is one mechanical, sample-verified sweep across `Create` call sites —
exactly the planset-1 cadence.

### 2. The back-reference is a `Context&`, captured at construction

Every resource that today reaches `Context::Instance()` in a destructor or member
gets a `Context& m_Context`, captured when it is created. Resources already have
deleted copy, always live behind `Ref`/`Unique`, and are never reassigned; the
ownership contract already states a resource must not outlive its context. A
reference encodes precisely that. (`Context*` is the fallback only if a resource
ever needs to be move-assigned — none do today.)

### 3. `Instance()` is a bridge, deleted last

`Context::Instance()` survives through plans 01–03 so **every plan is green**:
resource families convert one plan at a time, and converted/unconverted types
coexist because `Instance()` still answers for the unconverted ones. Plan 04
converts the last internal users and deletes the singleton as the capstone. Because
no example or test calls `Instance()` directly (only the engine's own backend
does — verified), the public break is confined to the `Create` signatures.

### 4. De-global enables, but does not mandate, multi-context

After plan 04 multiple `Context`s become *possible*, but `Application` still owns
exactly one (`Renderer::Context m_RenderContext`) and v1 stays single-context. The
`Veng.h` note drops the "singleton" wording and keeps the single-threaded contract.

## Plans

| # | Plan | Status | Depends on |
|---|------|--------|-----------|
| 01 | [Context back-reference (mechanism + internal de-global)](01-context-backreference.md) | proposed | — |
| 02 | [Explicit context in `Create` — buffers & images](02-explicit-create-buffers-images.md) | done | 01 |
| 03 | [Explicit context in `Create` — shaders, pipelines, descriptors](03-explicit-create-shaders-pipelines.md) | proposed | 01 |
| 04 | [De-globalize context internals & delete the singleton](04-deglobalize-internals.md) | proposed | 02, 03 |
| 05 | [In-process multi-case GPU integration suite (5b)](05-gpu-integration-suite.md) | proposed | 04 |
| 06 | [CI with a software Vulkan ICD + validation gate](06-ci-software-icd.md) | proposed | 05 |

## Dependency graph (for delegation)

```
01 back-reference  (mechanism — land first, main thread, validation-verified)
   │
   ├─► 02 explicit Create: buffers & images ──┐
   ├─► 03 explicit Create: shaders/pipelines ─┤  (parallelizable in principle;
   │                                          │   both migrate main.cpp, so
   │                                          │   sequence the merges)
   └──────────────► 04 internals + delete singleton  (capstone of de-global)
                          │
                          └─► 05 GPU integration suite (5b) ─► 06 CI + ICD
```

**Delegation guidance.** Plan **01 carries the design** (back-ref type, destructor
reroute, the retire path) and is barrier-/lifetime-adjacent — keep it on the main
thread and verify under `VE_DEBUG` (validation errors don't fail tests; a retire
regression is exactly what the smoke tests can miss). Plans **02 / 03** are
mechanical signature-and-call-site plumbing — delegatable to `model: sonnet`
subagents, but they both edit the hello-triangle `main.cpp`, so land them in
sequence, not concurrently, to avoid a migration collision. Plan **04** removes the
singleton and touches engine + docs — main thread. Plans **05 / 06** are the
testing half: 05 designs the per-case fixture (main thread); 06 is CI plumbing
(YAML + ICD provisioning), well-scoped for a subagent with review on the main
thread.

## Process discipline (from the future note)

> "Keep planset-1's cadence — small, sample-verified, per-plan increments —
> especially for de-global, where a big-bang sweep is most tempting and most
> dangerous."

Each plan: implement → migrate `examples/hello-triangle` in the same pass →
verify (clean build, `ctest` green, smoke binary writes a correct-sized PPM, and
for any plan touching the retire/lifetime path, a `VE_DEBUG` validation check) →
update this table → one commit per plan.

## On completion

This planset closes **future areas 3 and 5** in full. Update
[future/README.md](../future/README.md): strike area 3, mark area 5 done (5a =
planset-3, 5b + CI = planset-4), and re-cut the ordering diagram so the remaining
chain is `2 threading → 1 asset system` (plus independent area 4). Update the top
[plans/README.md](../README.md) index with the planset-4 entry.

## Conventions

- One commit per plan: `Plan NN: <summary>` with a `Co-Authored-By` trailer
  (`planset-4:` for the roadmap-only README/status edits).
- De-global is **behaviour-preserving** for plans 01–04: byte-for-byte the same
  Vulkan calls, only the path to the device changes. Any latent bug spotted while
  sweeping is noted separately, not fixed under a de-global plan.
- Test additions live under `tests/` (5b extends `tests/support/` from planset-3
  plan 06); CI config lives at the repo root / `.github/`.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and
> approved; `done` = landed and verified. Fleshed out and ordered before
> implementation, planset-1 style.
