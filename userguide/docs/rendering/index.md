# Rendering

veng gives you rendering at two levels.

The high-level [scene renderer](scene-renderer.md) takes a `Scene` and a `Camera`
and renders it through a deferred pipeline with shadows, SSAO, bloom, and culling,
controlled by a handful of settings.

The low-level [render graph](render-graph.md) lets you build your own passes by
declaring what each one reads and writes, and works out the barriers and layout
transitions for you. Use it when the scene renderer isn't the shape you need.

Underneath both:

- **[Bindless resources](bindless.md)** — set 0 is the engine's global resource
  table; textures, samplers, and material blocks are addressed by `u32` handle.
- **[Shaders & materials](materials.md)** — shaders authored in Slang and cooked
  to SPIR-V, and the thin handle-based `Material` on top.
