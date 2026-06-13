# 01 — Shader interface description + offline reflection

**Goal:** make every shader carry a serializable `ShaderInterface` — its
descriptor bindings, push-constant blocks, vertex inputs and specialization
constants — produced by reflecting SPIR-V **at build/import time**, not at
runtime. This is the foundation every other planset-2 plan derives from, and the
metadata the future asset importer and material editor will read.

**Supersedes:** planset-1 plan 12 (runtime spirv-reflect at `Shader::Create`).
Same derived-layout payoff, different timing: reflection is offline, the runtime
loads a description.

**Dependencies:** planset-1/06 (engine enums — `DescriptorType`, `ShaderStage`,
`Format` are the vocabulary the interface is expressed in). Complements 11.

## Why offline

- The runtime gains no reflection-library dependency and does no per-launch
  reparse; loading is deterministic.
- The interface becomes an asset artifact: the importer produces it, the editor
  and material tools consume it **without a live Vulkan device**.
- A shader's interface is fixed at compile time — reflecting it every run is
  wasted, identical work.

## Design

### The description type (public, engine types only, serializable)

```cpp
// Veng/Renderer/ShaderInterface.h
struct ShaderResource {           // one descriptor binding
    u32 Set; u32 Binding;
    string Name;
    DescriptorType Type;
    u32 Count;                    // array size; 0 = runtime/bindless array
    ShaderStage Stages;           // merged across stages at pipeline build
};

struct ShaderPushConstantMember { string Name; u32 Offset; u32 Size; /* + type */ };
struct ShaderPushConstantBlock {
    string Name; u32 Offset; u32 Size; ShaderStage Stages;
    vector<ShaderPushConstantMember> Members;   // for editor / named sets
};

struct ShaderVertexInput { u32 Location; string Name; Format Format; };

struct ShaderInterface {
    ShaderStage Stage;            // the stage this module is (vertex/fragment/…)
    string EntryPoint;
    vector<ShaderResource> Resources;
    optional<ShaderPushConstantBlock> PushConstants;
    vector<ShaderVertexInput> VertexInputs;   // populated for vertex stage
    // specialization constants: later
};
```

Serialization: a versioned binary (or JSON for debuggability) blob. Keep the
read/write in one place so the importer and the build step share it.

### Where reflection runs (bridge → asset)

- **Now (bridge):** extend `cmake/Shaders.cmake`. After `glslc` emits `foo.spv`,
  a small `veng-shaderc`-style step (a tiny host tool linking SPIRV-Reflect,
  built once) reflects it and writes `foo.spv.vrefl` next to it. SPIRV-Reflect
  is a build-tool dependency only — never linked into `veng` or exposed.
- **Later (asset system):** the shader importer runs the same reflection and
  bakes the `ShaderInterface` into the cooked shader asset. The runtime load
  path is identical; only the source of the blob changes.

Open question to settle in implementation: sidecar format (versioned binary vs
JSON) and whether the reflect tool is a standalone exe or a CMake-time script.
Recommendation: standalone exe emitting versioned binary, with a `--json` debug
mode.

### Runtime: Shader carries its interface

```cpp
struct ShaderInfo {
    string Name; path Path; string EntryPoint = "main";
    // Interface sidecar; defaults to <Path>.vrefl. Optional so binary/embedded
    // shaders can pass one inline.
    optional<path> InterfacePath;
};
struct ShaderBinaryInfo { ...; optional<ShaderInterface> Interface; };

const ShaderInterface& Shader::GetInterface() const;   // fatal if absent
bool Shader::HasInterface() const;
```

`Shader::Create(ShaderInfo)` loads the SPIR-V and, if present, the sidecar →
`ShaderInterface`. Missing sidecar is a `Result` error (plan-03 shape) only when
a later consumer needs it; a shader can still be created interface-less for the
hand-authored path. No SPIRV-Reflect in the runtime.

## Migration

This plan adds the description and the build step; it changes no call sites yet.
Plans 02–04 consume `GetInterface()`. The sample's shaders get sidecars from the
updated `Shaders.cmake` automatically.

## Acceptance

- Building the sample emits a `.vrefl` next to each `.spv`; `veng` links no
  reflection library and no public header includes one.
- `Shader::Create` populates `GetInterface()` with the correct bindings,
  push-constant block and vertex inputs for the sample's shaders (verify against
  the known triangle/composite layouts).
- The `ShaderInterface` round-trips through serialize → deserialize unchanged
  (a small CTest), proving it's asset-ready without a device.
