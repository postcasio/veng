# 05 — Pipeline attachment formats from render targets

**Goal:** stop restating color/depth attachment formats at pipeline creation to
match the images the render graph binds. Derive (or validate) them from the
target.

**Dependencies:** planset-1/08 (render graph), independent of 01.

## Current state

```cpp
m_TrianglePipeline = DynamicGraphicsPipeline::Create({
    .ColorAttachments = {{ .Format = context.GetOutputFormat() }},   // must match scene image
    ...
});
// composite:
.ColorAttachments = {{ .Format = context.GetSwapChainFormat() }},    // must match swapchain
```

The pipeline's attachment formats must equal the formats of the views the graph
renders into; the agreement is by hand and a mismatch is a validation error.

## Design

Dynamic rendering requires the pipeline know its attachment *formats* at creation
(not the actual images), so we can't remove formats entirely — but we can stop
hand-typing them:

- **Option A (derive at use):** a render-graph pass knows its color/depth
  attachment formats (from the declared `ImageView`s). Let a graphics pass
  validate the bound pipeline's formats against its attachments, asserting on
  mismatch — turns a silent validation error into a named engine assert. Keeps
  pipelines explicit but catches drift.
- **Option B (derive at creation):** let pipeline creation take attachment
  formats from a sample target / format set named once (e.g. a small
  `RenderTargetFormats` the scene and its pipelines share), so the format lives
  in one place.
- Recommendation: do A (cheap, high value — catches the bug class) and offer B's
  single-definition convenience where the sample shows the duplication.

Keep `BlendState` per color attachment explicit (it's intent, not derivable).

## Acceptance

- A pipeline whose attachment formats disagree with the graph pass it's used in
  fails a fatal assert naming the attachment and both formats (instead of a raw
  validation message).
- The sample defines each render target's format once; pipelines and the graph
  reference it rather than restating literals.
