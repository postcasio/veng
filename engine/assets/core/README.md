# veng core pack

Built-in vertex layouts (`canonical`, `screenspace`, `positiononly`) shipped
inside `libveng`. `core.vengpack.json` is cooked at build time and embedded as a
C array (`cmake/EmbedBinary.cmake`), then mounted automatically by
`AssetManager` before any user pack.

AssetIds here are minted with `vengc generate-id` — never hand-assigned. The
canonical layout's id is mirrored in `Renderer::Mesh::CanonicalLayoutId`; keep
the two in sync.
