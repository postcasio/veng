# Shaders & materials

Shaders are assets authored in **Slang**. Materials are a thin layer on top, checked
against their shader's parameters at cook time.

## Shaders: Slang in, SPIR-V out

A shader is a `*.shader.json` naming its `.slang` source, an entry point, and an
optional vertex layout. The cooker compiles it from Slang and reflects it; the
engine loads only the resulting SPIR-V and reflection, so it has no Slang
dependency and never compiles a shader at runtime. See
[Cooking asset packs](../assets/cooking.md).

## Materials

A material (`*.vmat.json`) names its shaders and lists its parameter values. The
cooker checks those values against the shader's parameters, so a wrong type or an
unknown field is caught at cook time.

At runtime a `Material` is thin — a shader handle, texture handles, and its
parameter block, all bound through [set 0](bindless.md).

### The parameter block

Each material has a single parameter block, laid out by shader reflection — its
texture handles and its scalar and vector parameters all share it, each at the
offset reflection assigned. There's no fixed engine struct; a material declares
whatever set of parameters its shader defines.

The block is buffered for frames in flight and kept mapped, so updating a parameter
or texture each frame is a direct write — no staging, no stalls:

```cpp
material->SetParam("BaseColor", vec4{1, 0.5f, 0.2f, 1});
material->SetTexture("Albedo", albedoTextureHandle);
```

`Material::GetFields()` exposes the reflected field list — the same schema the
[editor](../tools/editor.md) inspector reads.

## Material domains

A material's **domain** decides where it fits in the pipeline:

- **`Surface`** — an opaque mesh material. Its fragment shader writes g-buffer
  channels (albedo, world normal, packed ORM) rather than final color, and the
  geometry pass draws it per submesh.
- **`PostProcess`** — a fullscreen effect that outputs a single color and samples
  an upstream target bound each frame.

The `.vmat.json` `"domain"` key picks one (default `surface`), and the cooker
checks that the fragment shader's outputs match. The engine's own tonemap is a
post-process material.

### The standard vertex shaders

The engine ships the standard vertex shader per domain in the core pack —
`surface.vert` (canonical layout) and `fullscreen.vert` (screenspace), with the
shared bindless/material declarations in `material.slang`. A game references the
core `surface.vert` rather than shipping its own surface vertex stage.
