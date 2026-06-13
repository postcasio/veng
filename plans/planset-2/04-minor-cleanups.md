# 04 — Minor API cleanups

**Goal:** a grab-bag of small, independent rendering-API tidy-ups that don't
warrant their own plan and don't touch shaders. Each is opt-in; we land the ones
worth landing.

**Dependencies:** none. Shader-interface-dependent cleanups (e.g. dropping
`PipelineShaderStageInfo::Stage`, deriving vertex layouts) are deliberately
*excluded* — they belong to the shader/material phase.

## Candidates

### Frame API: `Command::BeginFrame/EndFrame`

`Renderer::Command` is a static two-method class (`BeginFrame` → `CommandBuffer&`,
`EndFrame`) sitting beside `Context`/`CommandBuffer`. It reads oddly as the frame
driver. Options: fold onto `Context` (`Context::BeginFrame()/EndFrame()`), or a
small RAII `Frame` scope (`auto frame = ctx.BeginFrame(); frame.Commands()...`).
Recommendation: move onto `Context` for discoverability; keep the behaviour.

### `VertexBufferLayout` vocabulary

Pre-planset-1/06 style: `const char*`/`std::string` names, a tiny
`VertexElementDataType { Float, Float2, Float3 }` enum, no engine `Format`. Bring
it to engine vocabulary (`Veng::string`, element type expressed as `Format`,
broader than float vectors). Pure mechanical modernization — deriving the layout
from a vertex struct or validating against a shader is a later (shader) phase.

### Other small consistency passes (enumerate during impl)

- Audit remaining `std::` types in public signatures that have `Veng::` aliases.
- `BlitImageInfo`/`BufferImageCopy`-style structs taking `Image&` vs `Ref<Image>`
  — consistency with the rest of the API.
- Any leftover doc/comment drift from planset-1's moves.

## Acceptance

- Each landed cleanup keeps the sample compiling and the headless smoke output
  unchanged.
- No behavioural change — these are surface/ergonomics only.
