# Plan 02 — Explicit context in `Create` — buffers & images

**Goal:** make the resource-creation dependency explicit for the buffer and image
families — `X::Create(Context&, const XInfo&)` — and migrate the sample and tests.
The first half of the public-API sweep, on the back-ref pattern proved in plan 01.

## Scope: this plan's families

- `Buffer` (`Buffer.h` / `Backend/Buffer.cpp`)
- `Image` (`Image.h` / `Backend/Image.cpp`)
- `ImageView` (`ImageView.h` / `Backend/ImageView.cpp`)
- `Sampler` (`Sampler.h` / `Backend/Sampler.cpp`)
- `TypedBuffers` (`TypedBuffers.h` — `VertexBuffer` / `IndexBuffer` /
  `UniformBuffer` / `StorageBuffer`, header-only wrappers over `Buffer::Create`)

Shaders, pipelines, and descriptors are plan 03.

## Work

1. **Change the factory signature.** For each type, prepend a `Context&` parameter:

   ```cpp
   static Ref<Buffer> Create(Context& context, const BufferInfo& info)
   {
       return Ref<Buffer>(new Buffer(context, info));
   }
   ```

   The private constructor gains the same leading `Context&`; its member-init list
   captures `m_Context(context)` instead of `m_Context(Context::Instance())` (the
   member and all its uses already exist from plan 01). Constructor-body
   `Context::Instance()` reads become the `context` parameter.

2. **Thread it through `TypedBuffers`.** The four typed wrappers forward to
   `Buffer::Create` — add `Context&` to each `Create` and pass it through. These
   are `include/`-only and are the sample's primary buffer entry points.

3. **`ImageView` from an `Image`.** `ImageView::Create` already takes an
   `ImageViewInfo` referencing its source image; add the explicit `Context&`
   alongside (don't try to fish the context out of the image — keep the parameter
   uniform across all factories).

4. **Migrate `examples/hello-triangle` and `tests/`.** Update every call site of
   these families to pass the context. The sample has it as
   `GetRenderContext()` / its `Application`'s `m_RenderContext`; the GPU tests
   already stand up a local `Context context;` (planset-3 plan 06's
   `tests/support/` helper) — thread that in. Plain mechanical edit:
   `Buffer::Create({...})` → `Buffer::Create(context, {...})`.

## Dependencies

Needs plan 01 (the `m_Context` member exists; this plan only changes where it's
captured). Independent of plan 03 in engine terms, **but both edit
`examples/hello-triangle/main.cpp`** — sequence the two merges; don't run them
concurrently against the same sample file.

## Acceptance

- Clean build, `ctest` green, smoke binary writes a correct-sized PPM.
- No buffer/image-family `Create` reaches `Context::Instance()` — they take it as
  a parameter. (`Instance()` still exists for the unconverted families and the
  context internals; that's expected until plan 04.)
- Validation check under `VE_DEBUG` for `headless_smoke` (the image clear/download
  path) — unchanged from plan 01.

## Notes

- **Parameter order:** `Context&` first, `const XInfo&` second — uniform across the
  whole API so the sweep is muscle-memory and `grep`-auditable.
- Keep `XInfo` exactly as is — the context is a creation *dependency*, not resource
  *data*, so it stays out of the designated-initializer info struct.
- This is a breaking change to the public surface; that's the point. There is no
  deprecation shim — veng has no external consumers yet, and a parallel old/new
  `Create` would defeat the de-global by keeping the implicit path alive.
