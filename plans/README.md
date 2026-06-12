# veng API rework — plan index

Plans derived from [API_RECOMMENDATIONS.md](../API_RECOMMENDATIONS.md) (2026-06).
Numbered in intended execution order; each file states its actual dependencies, so
some can be reordered or interleaved where the dependency notes allow.

Decisions baked into these plans:

- **Ownership (2.1):** full deferred-destruction queue. `SubmitResource` and the
  `Ref<void>` keep-alive lists are deleted, not deprecated.
- **Descriptors (2.2):** typed writers with layout-inferred types (steps 1+2).
  Bind groups remain a future direction, not planned here.
- **Errors:** no exceptions. Asserts are fatal (log + debug-break/abort);
  genuinely recoverable paths return `Result<T>`. No `Veng::Error` exception type.
- **Insulation (2.3):** full — vocabulary enums, public/backend header split,
  intent-free barriers (via render graph), GLFW/nfd out of public headers.
- **Scope:** veng and the in-repo sample app (`examples/hello-triangle`). The
  sample is the reference consumer and gets migrated in the same pass as each
  breaking change.

| # | Plan | Status |
|---|------|--------|
| 01 | [Part 1 targeted fixes](01-part1-targeted-fixes.md) | done (2026-06-12) |
| 02 | [Window ownership & engine teardown](02-lifecycle-window-teardown.md) | not started |
| 03 | [Error handling & logging](03-error-handling-logging.md) | not started |
| 04 | [Deferred destruction queue](04-deferred-destruction.md) | not started |
| 05 | [Descriptor set typed writers](05-descriptor-typed-writers.md) | not started |
| 06 | [Vocabulary enums (Vulkan + GLFW + nfd)](06-vocabulary-enums.md) | not started |
| 07 | [Public/backend header split](07-public-backend-header-split.md) | not started |
| 08 | [Minimal render graph](08-render-graph.md) | not started |
| 09 | [ImGui as an optional module](09-imgui-module.md) | not started |
| 10 | [Headless context](10-headless-context.md) | not started |
| 11 | [Typed buffers](11-typed-buffers.md) | not started |
| 12 | [Shader reflection](12-shader-reflection.md) | not started |

Dependency sketch:

```
01 ──────────────────────────────┐
02 ──► 10                        │
03 (early: later plans write     │
    error paths in new style)    │
04 ──► 05                        │
06 ──► 07 ──► 08                 │
06 ──► 11                        │
09 ──► 10                        │
05 ──► 12                        │
```

Cross-plan notes:

- 01 adds *interim* asserts to `DescriptorSet` that 05 deletes again — that's
  intentional (01 ships immediately, 05 is a redesign).
- 08 (render graph) is what finally removes `ImageBarrier`/layouts from the
  public API; 07 leaves barriers as the one acknowledged hole until then.
- The `Veng::string`/`vector`/... alias question from Part 1 is handled in 07,
  where the public headers get reworked anyway.
- Thread-safety contract (single-threaded v1) gets stated in docs as part of 07,
  which is where API documentation gets touched wholesale.
