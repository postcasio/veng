# material domains + shader-graph codegen (DRAFT / vision)

Direction overview for [future/README.md](README.md) **area 13**. Direction, not a
plan — each piece below becomes its own planset when taken up.

## Where this is going

veng's material system is **temporarily** a parameter-binding system. A `.vmat`
declares an ordered, typed field list bound to a **hand-authored** Slang shader,
and the node material editor (planset-15) authors that binding visually — a
`TextureSample` chooses which texture fills a texture field, a `Param` fills a
value field, and `MaterialOutput`'s pins are **derived from the loaded shader's
reflected `Material::GetFields()`**. The graph is a wiring diagram over a shader
that already exists.

The committed end-state is **shader-graph codegen**: the node graph **generates**
the Slang fragment source. A material's shading is authored *in the graph*, not in
a hand-written shader the graph merely feeds. planset-15's param-binding is the
way-station that proved the graph → cook → hot-reload → preview loop end-to-end; it
is not the destination.

Two organizing ideas carry it there, independent enough to land in order:

1. **Material domains** — at least **Surface** and **PostProcess**. *Prioritized*,
   and does not require codegen.
2. **Node→Slang codegen** — the graph emits the shader. The larger follow-on; the
   node model is reshaped to receive it.

## 1. Material domains (prioritized)

Today a `Material` is hardwired to one implicit domain: an **opaque surface** whose
fragment shader writes the g-buffer (`GBufferOutput { Albedo, Normal }`, the fixed
contract the geometry pass and every material pipeline agree on). There is no domain
concept — the g-buffer output contract is baked into the `Material` ↔ geometry-pass
agreement.

A **material domain** is a first-class property of a material that selects four
things, leaving the rest of the material system shared:

- **Outputs** the material writes — Surface writes the g-buffer channels;
  PostProcess writes a single final color.
- **Inputs** it may read — Surface reads interpolated vertex attributes / world
  position; PostProcess reads screen UV plus scene-texture / g-buffer samplers.
- **Pipeline shape** — Surface compiles to a mesh pipeline with a vertex layout,
  drawn into the g-buffer; PostProcess compiles to a fullscreen-triangle pipeline
  into one color target.
- **Invocation** — Surface is drawn by the geometry pass per submesh; PostProcess is
  invoked by the post chain (a `ScenePass`) with the framebuffer bound as input.

This is the standard cross-engine factoring: Unreal's `MaterialDomain`
(`Surface` / `PostProcess` / `UI` / …), Unity's Shader Graph surface-vs-fullscreen
targets, Godot's `shader_type`. The unit everything shares — the parameter schema
(`MaterialData` + `MaterialParams`, planset-15 plan 00), bindless handles, the
`.vmat` authoring, `Material::GetFields()`, the editor inspector — stays one system;
the **domain** is the tag that picks the I/O template, the pipeline builder, and the
invocation site.

What the runtime needs for the PostProcess domain is a **fullscreen material pipeline
path** in `SceneRenderer`: a `ScenePass` that builds a pipeline from a postprocess
material's fragment shader against a single color target, samples upstream targets
through set-0 bindless, and exposes the material's authored `MaterialParams` as the
tunable knobs. This is exactly the **exposure / tonemap-curve / color-grading /
bloom-threshold** class of effect — the authorable post stack named under
[area 8](README.md#8-scene-renderer--render-pipeline-architecture--remaining-the-über-pipeline-batteries),
now expressed as materials instead of bespoke passes.

**The line that survives.** Fixed *plumbing* composites stay hardcoded engine passes
even after domains exist — the swapchain composite (`SwapChainCompositePass`: scene
behind, ImGui over) has no authorable surface and is not content, exactly as the
final backbuffer blit is C++ in the engines above. PostProcess **materials** are for
*tunable effects* with exposed parameters; *plumbing* is not a material.

## 2. Shader-graph codegen + the node reconsideration

The current node catalog is shaped for param-binding, not codegen, and that shows in
concrete ways worth changing as the foundation is laid:

- **`MaterialOutput` reflects a hand-authored shader.** Its input pins are derived
  from the loaded material's `GetFields()` — i.e. the graph conforms to a shader that
  pre-exists. Under codegen there is no pre-existing shader; the graph **is** the
  source. `MaterialOutput` becomes a **domain-driven sink**: its pins are the domain's
  fixed output contract (Albedo / Normal / Metallic / Roughness for Surface; Color for
  PostProcess), and compile assembles the fragment entry that writes them. The
  `MaterialShaderInterface` (fields fed *in* to compile) inverts — the graph generates
  the interface rather than consuming it.
- **Nodes route; they do not compute.** `TextureSample` → output pin currently means
  "bind this texture to this field," and a `Param` means "fill this field with this
  constant." There is no shading math in the graph — the hand-authored shader does it.
  Under codegen every node is an **expression emitter**: `TextureSample` emits
  `tex.Sample(samp, uv)`, a `Multiply` emits `a * b`, and the output node concatenates
  the emitted expressions into the entry point. (The "basic math" nodes already hint at
  this — a multiply node has *no meaning* in a pure binding model except constant
  folding, so the catalog was already half-built for codegen.)
- **`Param` gains a const-vs-exposed distinction.** A constant folds inline into the
  emitted Slang; an **exposed** param becomes a generated `MaterialParams` uniform —
  runtime-tweakable, the same authored-block planset-15 plan 00 created. So the
  parameter block survives codegen; it is now *generated* rather than authored against.
- **Compile target changes** from "a `.vmat` field list bound to a shader" to
  "**generated Slang source** + the exposed `MaterialParams` schema + the texture
  bindings." The cook then compiles the generated Slang the same way it compiles any
  hand-authored shader (Slang → SPIR-V + offline reflection) — no new runtime path; the
  engine still loads plain SPIR-V and a reflected interface.

**What is already codegen-ready** (the topology and catalog core, planset-15 Layers
1–2, do not change): the generic `NodeGraph` topology, typed pins over the builtin
`TypeId` space, coercion recorded **on the link** at compile (splat / truncate — an
emitter reads it directly), reflected node-property structs walked like ECS
components, and the JSON graph round-trip embedded under `"_editor"`. The reshaping
is concentrated in **Layer 3** (the material catalog + the compile target) plus the
new **domain** concept that spans the runtime, the cooker, and the editor.

## What it builds on

- **planset-15** — the node graph surface (`VengEditor/NodeGraph/`), the material
  catalog, and the engine-supplied + authored `MaterialParams` split (plan 00) the
  generated uniforms reuse.
- **planset-12 / area 8** — the `SceneRenderer` + `ScenePass` mechanism the
  PostProcess domain's fullscreen pipeline path slots into, alongside the named post
  stack.
- **planset-5** — the material runtime (handle + texture handles + SSBO entries,
  set-0 bound) and the cooker's Slang compile + offline reflection that the generated
  Slang feeds into unchanged.

## Supersedes

**planset-15 decision 9** ("v1 binds parameters; node→Slang codegen is deferred; the
node/property model is *shaped so codegen can layer on*"). Codegen is now **committed
direction**, not a possibility the model merely tolerates, and the node catalog is to
be **reshaped toward it** — domain-driven output, expression-emitting nodes,
const-vs-exposed params — rather than only kept compatible with it.
