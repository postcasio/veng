# The scene renderer

`SceneRenderer` renders a `Scene` from a `Camera` through a deferred pipeline and
gives you back a result you can sample. It's built on a
[render graph](render-graph.md) internally, but you don't need to work with that to
use it.

`Create(const SceneRendererInfo&)` is the factory.

## What it draws

A metallic-roughness deferred pipeline:

- a **three-target g-buffer** — albedo, world-space normal, and packed
  occlusion/roughness/metallic + emissive, plus depth;
- **tangent-space normal mapping** in the geometry pass;
- a fullscreen **Cook-Torrance** lighting pass (GGX specular and Lambert diffuse)
  over any number of directional, point, and spot lights;
- a **tonemap** to the final output.

A handful of extras layer on top, each switched by a setting:

- **shadows** — cascaded shadow maps for the directional light, and a shared atlas
  for point and spot lights;
- **SSAO**, folded into the ambient term;
- **bloom**, a compute mip-pyramid before tonemap;
- **GPU-driven occlusion culling** (see [Culling](#culling) below).

## The lifecycle

The API is split by how often each piece of state changes, which is what keeps
per-frame rendering free of allocation:

| Call | When | What it does |
| --- | --- | --- |
| `Create(info)` | once | Allocates the targets and pipelines and builds the graph. |
| `Resize(extent)` | on resize | Recreates the size-dependent images and rebuilds. |
| `Configure(settings)` | on a settings change | Recreates affected resources and rebuilds the pass set. |
| `Execute(cmd, view)` | every frame | Renders this frame. Never reallocates or rebuilds. |
| `GetOutput()` | as needed | The sampleable result. |

!!! warning "`Resize` and `Configure` replace the output image"
    Both retire the old output and create a new one, so anything caching a
    `TextureHandle` or ImGui texture from `GetOutput()` must re-fetch it afterward.

## Settings vs per-frame values

There are two kinds of knob, and the split matters for performance.

**Settings** (`SceneRendererSettings`) change the *shape* of the pipeline — which
passes exist and how big their targets are. Changing one means a `Configure`, which
rebuilds the graph. These include the shadow, SSAO, and bloom toggles, the shadow
and bloom resolutions, the cull mode, and the debug-view selector.

**Per-frame values** ride on the `SceneView` you pass to `Execute`, so adjusting
them costs nothing: exposure, the bloom threshold/intensity/radius, the camera, and
the lights.

```cpp
SceneRendererSettings settings{
    .Bloom   = true,
    .Shadows = true,
    .AO      = true,
};
renderer->Configure(settings);     // rebuilds — do this when settings change

// every frame — cheap:
renderer->Execute(cmd, SceneView{
    .World  = scene,
    .Camera = camera.GetView(),
    .Delta  = delta,
});
```

The debug-view selector is useful while building a scene: it can show any g-buffer
channel, the SSAO or shadow targets, or the bloom pyramid instead of the final
image.

## Culling

The renderer culls at submesh granularity using a bounding-volume hierarchy. It
builds the tree once from the scene and queries it for every view that frame — the
camera, each shadow cascade, each shadowed light — rather than re-scanning the
scene per view. The tree only rebuilds when the scene actually changes, so a static
scene queries a stable one.

Culling is conservative — it may draw something it could have skipped, but never
skips something visible — so the image is identical whether or not it's on; only
the number of draw calls differs.

`CullMode::GPU` adds a compute occlusion test against the previous frame's depth
and issues the survivors as indirect draws. On a device that can't support it, the
renderer logs once and falls back to CPU culling; `GetActiveCullMode()` tells you
which is actually running.

## When to drop down a level

`SceneRenderer` is configured by its settings, not extended with custom passes. If
you need a pass graph it doesn't provide, build your own with the
[render graph](render-graph.md); the sample does this to composite the scene and
the UI onto the swapchain. `GetOutput()` is sampleable in the same frame it's
written.
