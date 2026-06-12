# 11 — Typed buffers

**Goal:** `VertexBuffer<V>`, `IndexBuffer`, `UniformBuffer<T>`, and
`StorageBuffer<T>` wrappers over `Buffer` that fix usage flags and element
types at compile time — removing `BufferUsage` from most call sites and
catching stride/type mismatches statically.

**Dependencies:** 06 (engine `BufferUsage` exists; these wrappers then hide it).
Independent of 07/08; slots anywhere after 06.

## Current state

`Buffer` (`include/Veng/Renderer/Backend/Buffer.h`) is raw bytes:
`BufferInfo{ Name, Size, Usage }`, `Upload(span)`, `Download()`. Every call
site restates usage flags and computes byte sizes by hand;
`CommandBuffer::BindVertexBuffer`/`BindIndexBuffer` (`CommandBuffer.h:74-75`)
accept any buffer regardless of usage or element type, and index type
(u16/u32) is implicit.

## Design

Header `Veng/Renderer/TypedBuffers.h`, thin layer over `Buffer` (composition,
not inheritance — each wrapper holds `Ref<Buffer>` / is convertible to one):

```cpp
template <typename V>
class VertexBuffer {
public:
    static VertexBuffer Create(string_view name, usize vertexCount);
    void Upload(std::span<const V> vertices, usize firstVertex = 0);
    [[nodiscard]] usize GetVertexCount() const;
    [[nodiscard]] const Ref<Buffer>& GetBuffer() const;
};

class IndexBuffer {            // not a template: index width is a vocab choice
public:
    enum class Type : u8 { U16, U32 };
    static IndexBuffer Create(string_view name, usize indexCount, Type type = Type::U32);
    void Upload(std::span<const u32> indices, usize firstIndex = 0);
    void Upload(std::span<const u16> indices, usize firstIndex = 0);  // asserts type
    [[nodiscard]] usize GetIndexCount() const;
};

template <typename T>
class UniformBuffer {          // requires std::is_trivially_copyable_v<T>
public:
    static UniformBuffer Create(string_view name);
    void Upload(const T& value);
};

template <typename T>
class StorageBuffer {
public:
    static StorageBuffer Create(string_view name, usize elementCount);
    void Upload(std::span<const T> elements, usize firstElement = 0);
};
```

- Usage flags are fixed per wrapper (`Vertex|TransferDst`, etc.) and disappear
  from call sites; raw `Buffer::Create` stays for staging/exotic cases.
- `static_assert(std::is_trivially_copyable_v<T>)` in all wrappers. Document
  (not solve) std140/std430 alignment: a comment + `alignas` guidance on
  `UniformBuffer<T>`; reflection (plan 12) can validate against SPIR-V later.
- Partial-update `Upload(..., firstVertex/firstElement)` needs a
  `Buffer::Upload(span<const u8>, u64 offset)` overload — small addition to
  `Buffer`.
- `CommandBuffer` gains typed overloads:
  `BindVertexBuffer(const VertexBuffer<V>&)` (template, header-only),
  `BindIndexBuffer(const IndexBuffer&)` (passes the right `vk::IndexType` —
  today's untyped path presumably hardcodes one; check `CommandBuffer.cpp`).
  Untyped `Ref<Buffer>` overloads remain for now.
- `DescriptorSet::Write(binding, const UniformBuffer<T>&)` /
  `(..., const StorageBuffer<T>&)` convenience overloads forward to the
  plan-05 writers and assert the layout type matches the wrapper.

## Migration

Opportunistic, not forced: the sample app converts buffer call sites as
touched. Inside veng, any internal buffer uses (ImGui layer staging, image
upload/download staging in `Image.cpp`/`Buffer.cpp`) stay raw — staging is the
raw-bytes case.

## Acceptance

- Creating and drawing with `VertexBuffer<Vertex>` + `IndexBuffer` requires no
  usage flags or byte arithmetic at the call site.
- `BindIndexBuffer(IndexBuffer)` emits the correct `vk::IndexType` for both
  widths.
- Uploading the wrong element type is a compile error; uploading u16 indices
  into a U32 index buffer is a fatal assert.
