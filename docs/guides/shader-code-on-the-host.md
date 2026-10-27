# Shader code that also runs on the host

A procedural function written in Slang — a noise, a fractal sum, a height or distance function —
usually runs only on the GPU. When the CPU needs the same answer (a collider built from the
surface the GPU draws, an offline generator, a query from a process with no GPU at all), the usual
fix is a hand-kept C++ copy, and the two drift. veng compiles **one Slang source for both**: the
cook compiles it to SPIR-V as it does every shader, and `veng_add_host_shader_module` compiles it
to C++ in an ordinary static library. A function written generically over the floating-point type
runs in `float` on the GPU and can be instantiated in `double` on the host.

## The pattern: a shared file, and a small exports file

```
shaders/
  height_fields.slang        # generic functions; GPU shaders #include it
  height_fields_host.slang   # host only: includes it, exports one wrapper per instantiation
```

The shared file is plain Slang with no host-only syntax. Write field code generically:

```slang
// height_fields.slang
vector<T, 4> Ridge<T : __BuiltinFloatingPointType>(vector<T, 3> p) { ... }
```

The exports file declares the host's entry points, one per instantiation, each
`export __extern_cpp` so its emitted name is not mangled:

```slang
// height_fields_host.slang
#include "height_fields.slang"

export __extern_cpp float4 RidgeF32(float3 p) { return Ridge<float>(p); }
export __extern_cpp double4 RidgeF64(double3 p) { return Ridge<double>(p); }
```

GPU shaders include `height_fields.slang` as they include any helper and never see the exports
file. In CMake:

```cmake
veng_add_host_shader_module(height_fields_host
        MODULE shaders/height_fields_host.slang
        NAME HeightFields)
target_link_libraries(my_tool PRIVATE height_fields_host)
```

and in C++:

```cpp
#include <HeightFields.h>

const glm::dvec4 ridge = HeightFields::RidgeF64(glm::dvec3(x, y, z));
```

`SHADER_INCLUDE <dir>` overrides the engine core shader dir on the search path (by default
veng's own), so a module that `#include`s `Veng/...` resolves exactly as it does in the cook.
`DEPENDS` adds regeneration triggers; the files the module includes are already tracked through
the depfile `vengc compile-host` writes. The helper works from an in-tree build, a build tree and
an installed SDK alike; the library links glm and nothing else, so the process using it carries
no Slang and no Vulkan.

## What may cross the boundary

An exported function takes and returns `float`, `double`, `int` and `uint` scalars and their 2- to
4-component vectors, which arrive in C++ as `float`/`double`/`std::int32_t`/`std::uint32_t` and
the matching `glm::vec*`/`dvec*`/`ivec*`/`uvec*`. Anything more comes back through `out` or `inout`
parameters, which become references. A struct does not cross: its emitted C++ name is mangled.
`vengc compile-host` refuses any other type with an error naming the function and parameter.

## What a host module may not contain

A host module is a **pure function of its arguments**:

- **No global shader parameters** — no textures, buffers, samplers or uniforms. The compile
  refuses a module that declares one, naming it. Resources are GPU-only.
- **No entry points.** A host module is a library of functions, not a CPU backend for kernels.
- **No derivatives** (`ddx`, `ddy`, `fwidth`): the emitted C++ has nothing to call, so the
  library fails to compile.
- **No wave or subgroup operations**: they compile to their single-lane value on the host, which
  differs from the GPU's.

## Writing field code that agrees on both targets

- **Integer arithmetic that may overflow must be unsigned.** Signed overflow wraps on the GPU and
  is undefined behaviour in the emitted C++, where the optimiser may assume it never happens, so
  a hash written in `int` can disagree between the targets. One written in `uint` cannot.
- **Keep float-to-integer conversions in range.** An out-of-range conversion saturates on the GPU
  and is undefined in C++; reduce a domain that would exceed `2^31` before converting.
- **Spell a generic float-to-int conversion through an interface.** `int(x)` does not type-check
  on a `T : __BuiltinFloatingPointType`, and `__intCast<int>(x)` aborts the SPIR-V backend, so
  extend `float` and `double` with a conversion of your own:

  ```slang
  interface ILatticeReal { int ToLatticeInt(); }
  extension float : ILatticeReal { int ToLatticeInt() { return int(this); } }
  extension double : ILatticeReal { int ToLatticeInt() { return int(this); } }

  vector<T, 4> Noise<T : __BuiltinFloatingPointType & ILatticeReal>(vector<T, 3> p) { ... }
  ```

- **The targets agree to rounding, not to the bit.** The Metal compiler runs with fast math and
  contracts multiply-adds; the host library is built with neither, so an arm64 host and an x64 host
  evaluate the same expressions the same way, and the transcendental implementations differ.

## How closely they agree

Measured once, over 6,016 stratified points (lattice points, negative cells, one ulp either side of
lattice planes, and a spread over `|p| <= 256`), on a gradient noise with an analytic gradient and
a fractal sum of up to 16 octaves, with Slang 2026.18.3 and MoltenVK 1.4.2 under its default fast
math:

| octaves | host `float` against GPU: value | gradient |
|---|---|---|
| 1 | 8.1e-7 | 2.6e-6 |
| 6 | 8.3e-7 | 3.8e-6 |
| 16 | 8.9e-7 | 5.3e-6 |

A `double` host evaluation sits as close to the GPU (6.4e-7 on the value at 16 octaves): at that
domain the gap is the GPU's own `float` rounding. The engine's agreement test
(`tests/gpu/host_shader_agreement.cpp`) holds the fixture to **1e-5 on the value and 1e-4 on the
gradient**, which is the bound a consumer may rely on for code of that shape. A consumer whose
function is longer, or runs over a larger domain, measures its own bound the same way — one
dispatch, one readback, the worst error accumulated.

Slang is not vendored, so the emitted C++ is the installed compiler's. The helper warns at
configure time when the installed Slang is not the version the bound was measured with.
