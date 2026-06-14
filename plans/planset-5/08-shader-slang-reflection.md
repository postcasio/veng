# Plan 08 — Shader via Slang + offline reflection → `ShaderInterface`

**Goal:** the shader asset, with **reflection moved offline**. The cooker compiles
a **Slang** source to SPIR-V and reflects it into a serializable `ShaderInterface`
(descriptor bindings, push-constant blocks, vertex inputs); the cooked shader blob
is `interface + SPIR-V`. The engine registers a `ShaderAsset` loader that creates a
`Shader` from the SPIR-V and carries the `ShaderInterface`, from which
descriptor/pipeline layouts are **derived instead of hand-declared**. This is where
the long-deferred shader-reflection work (planset-1/12 + the shader parts of
planset-2) finally lands.

## Why this is its own plan

Reflection is the keystone the material plan stands on: a `Material` validates its
params/textures against the shader's interface and builds its layouts from it.
Settling the interface representation, the Slang toolchain, and the
reflection-to-layout derivation here — before materials — keeps plan 09 about
materials, not shader plumbing. It's also independent of textures/meshes, so it can
proceed in parallel with 06/07.

## The interface (serialized at cook time, never derived at runtime)

```cpp
struct ShaderInterface
{
    vector<DescriptorBinding> Bindings;       // set, binding, type, count, stageMask
    vector<PushConstantBlock> PushConstants;  // offset, size, stageMask (validated ≤128B, planset-2/01)
    VertexBufferLayout        VertexInputs;   // from the vertex stage
    // set 0 is flagged engine-provided (the bindless registry, plan 05);
    // the author never declares it — recognized + skipped here.
};
struct CookedShaderHeader { u32 InterfaceBytes; u32 SpirvBytes; /* + interface + spirv */ };
```

`ShaderInterface`'s serialization format is defined in `assetformat` (plan 02's
reserved fields) using underlying-integer enums (the cycle rule); the engine
bridges to `Renderer::` enums on load.

## Cook side (`libveng_cook`)

- A `ShaderImporter : AssetImporter`. Two input forms:
  - `{ "type": "shader", "source": "shaders/brick.slang", "entry": ["vsMain","fsMain"] }`
    → invoke **Slang** (`slangc`/the Slang API) → SPIR-V.
  - `{ "type": "shader", "spirv_b64": "…" }` → precompiled SPIR-V, base64-decoded
    (the editor/inline path; also how materials inline shaders, plan 09).
- **Reflect the final SPIR-V with SPIRV-Reflect** — one reflection path for both
  forms (Slang-compiled and inline-precompiled), so the cooker doesn't depend on
  Slang's reflection API for the inline case. Produce `ShaderInterface`; recognize
  `set 0` as engine-provided and exclude it from the declared bindings.
- Emit `CookedShaderHeader` + serialized interface + SPIR-V.
- New cooker deps (pinned, cooker-only): **Slang** (prefer the prebuilt release;
  document the toolchain requirement) and **SPIRV-Reflect**.

## Load side (`libveng`)

```cpp
struct ShaderAsset { Ref<Shader> Module; ShaderInterface Interface; };
```

- A `ShaderLoader : AssetLoader`: read the blob → `Shader::Create` from the SPIR-V
  (today's runtime SPIR-V loader, unchanged) + deserialize the `ShaderInterface`.
- **Layouts derived from reflection.** Add a path that builds
  `DescriptorSetLayout` / `PipelineLayout` from a `ShaderInterface` (sets ≥ 1;
  set 0 reserved) instead of the hand-declared `…Info` structs. This is the
  consumed half of planset-1/12 and the missing half of planset-2:
  - descriptor/pipeline layouts from the interface, **with set 0 supplied by the
    `BindlessRegistry`** (plan 05's `GetSet0Layout()`) and author bindings in
    sets ≥ 1;
  - **name-based binding** — resolve a binding by name to its set/binding;
  - **vertex layout validation** — a mesh's `VertexBufferLayout` checked against
    the shader's `VertexInputs` at material/draw setup (a loud mismatch, tightening
    plan 07's canonical-layout check).

## Work

1. Cooker: Slang + SPIRV-Reflect deps; `ShaderImporter` (both input forms) +
   register; a fixture `.slang` shader and a precompiled-SPIR-V fixture.
2. `assetformat`: finalize the `ShaderInterface` + `CookedShaderHeader`
   serialization (the reserved fields from plan 02).
3. Engine: `ShaderAsset`, `ShaderLoader`, register; the reflection→layout builder
   + name-based binding + vertex-layout validation, with the enum bridges.
4. Tests: cook a known shader, load it, assert the reflected bindings/push
   constants/vertex inputs match expectation; assert a layout built from reflection
   matches a hand-declared equivalent for the sample's shader; a vertex-layout
   mismatch raises a loud error.

## Dependencies

Plans 02 (interface serialization), 03 (importer table), 04 (loader table), and 05
(set 0 from the registry). Independent of 06/07. **Blocks 09** (material needs the
interface + derived layouts).

## Acceptance

- Clean build, `ctest` green incl. the reflection tests.
- A shader cooked from Slang loads and yields a correct `ShaderInterface`; layouts
  built from it drive a working pipeline (proven via the sample in 09/10).
- **Validation-clean** under `VE_DEBUG` for any pipeline built from reflected
  layouts.

## Notes

- **Set 0 is the bindless registry** (plan 05) — recognizing/excluding it in
  reflection is the contract that lets a reflected pipeline layout splice in the
  registry's set-0 layout while author bindings live in sets ≥ 1.
- Slang-native reflection (parameter blocks, etc.) is a possible later upgrade; v1
  uses SPIRV-Reflect uniformly for the simpler, single-path story.
- Push-constant blocks are validated ≤128B at cook time too (the planset-2/01 cap),
  so an over-budget shader fails the *cook*, not a runtime assert.
