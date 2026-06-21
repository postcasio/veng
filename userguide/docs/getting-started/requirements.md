# Requirements

veng needs only a few things installed:

- A **C++26**-capable compiler.
- **CMake 4.1** or newer.
- A **Vulkan SDK**. On macOS this is **MoltenVK**, veng's primary target.
- **GLFW**, **glm**, and **zlib**, found via `find_package`.

Everything else — fmt, Vulkan Memory Allocator, Dear ImGui, imnodes, stb,
tinyexr, Native File Dialog, and the cooker's heavier tools (assimp, Slang,
nlohmann/json) — is downloaded and version-pinned by CMake during configuration.
There's nothing else to install.

The cooker's tools (assimp, Slang, JSON) build only when you build the cooker, and
never end up linked into the runtime: the engine loads a binary asset pack and
never parses a source asset. See [Cooking asset packs](../assets/cooking.md).

## Optional

`doxygen`, if installed, enables the API-reference build — the `docs` CMake target
and this guide's [API reference](../api/index.md) section. It isn't needed to build
or run the engine.
