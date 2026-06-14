#pragma once

#include <Veng/Asset/Types.h>

// Per-type cooked-blob header layouts. Each header is
// followed in the blob by the type's payload, as noted per struct.
//
// The cycle-avoidance rule (load-bearing): enum-typed fields below (pixel
// Format, index Type, shader stages, ...) are stored as their *underlying
// integer type* — assetformat deliberately does not include the engine's
// renderer vocabulary header. The engine loader casts to the corresponding
// Veng::Renderer enum guarded by a static_assert/VE_ASSERT (loud one-line fix
// on drift, per house style). This keeps assetformat standalone and the
// cooker buildable without the engine.
//
// These headers are the shared contract between the cooker (production) and the
// engine loaders (consumption).

namespace Veng
{
    // Texture: stb-decoded, single mip.
    // Sampler fields mirror Veng::Renderer::SamplerInfo, stored as underlying
    // integer/float types per the cycle-avoidance rule above. Followed by
    // Width * Height * bytes-per-pixel(Format) raw pixel bytes.
    struct CookedTextureHeader
    {
        u32 Format = 0; // underlying Renderer::Format
        u32 Width = 0;
        u32 Height = 0;
        u32 MipCount = 1;

        u32 MinFilter = 0;    // underlying Renderer::Filter
        u32 MagFilter = 0;    // underlying Renderer::Filter
        u32 MipmapMode = 0;   // underlying Renderer::MipmapMode
        u32 AddressModeU = 0; // underlying Renderer::AddressMode
        u32 AddressModeV = 0; // underlying Renderer::AddressMode
        u32 AddressModeW = 0; // underlying Renderer::AddressMode
        u32 AnisotropyEnabled = 0; // bool
        f32 MaxAnisotropy = 1.0f;
    };

    // Mesh: interleaved vertices + indices in a declared layout. The
    // blob is, in order:
    //   CookedMeshHeader
    //   CookedVertexAttribute[AttributeCount]   — the interleaved layout
    //   CookedSubMesh[SubMeshCount]             — draw ranges + material ids
    //   vertex bytes  (VertexCount * VertexStride)
    //   index bytes   (IndexCount * (IndexType == U16 ? 2 : 4))
    // The attribute descriptor records the on-disk interleaved format so the
    // loader can validate it against the engine's canonical VertexBufferLayout
    // (a loud Corrupt error on mismatch, not silent UB). v1 is a fixed canonical
    // layout (position/normal/tangent/uv), but the descriptor is stored
    // self-describingly so a later layout change is a format-version bump, not a
    // silent reinterpretation.
    struct CookedMeshHeader
    {
        u32 VertexStride = 0;
        u32 VertexCount = 0;
        u32 IndexCount = 0;
        u32 IndexType = 0; // underlying Renderer::IndexType
        u32 SubMeshCount = 0;
        u32 AttributeCount = 0;
    };

    // One interleaved vertex attribute, in layout order. Format is stored as the
    // underlying Renderer::Format integer (cycle-avoidance rule above); Offset
    // is its byte offset within a vertex (redundant with the running stride, but
    // stored so validation is a direct field-by-field compare).
    struct CookedVertexAttribute
    {
        u32 Format = 0; // underlying Renderer::Format
        u32 Offset = 0;
    };

    // One draw range within a cooked mesh's index buffer, with the material it
    // was authored against (AssetId, a forward reference resolved at load
    // time).
    struct CookedSubMesh
    {
        u32 IndexOffset = 0;
        u32 IndexCount = 0;
        u64 MaterialId = 0; // AssetId
    };

    // Names in cooked blobs are fixed-size and nul-terminated, truncated at
    // ShaderNameCapacity - 1 — generous for GLSL/Slang identifiers. Shared
    // by shader bindings/blocks/attributes and vertex-layout element names.
    inline constexpr usize ShaderNameCapacity = 64;

    // VertexLayout: a named list of vertex-buffer elements that shaders and
    // pipelines reference by AssetId. The blob is, in order:
    //   CookedVertexLayoutHeader
    //   CookedVertexLayoutElement[ElementCount]
    struct CookedVertexLayoutHeader
    {
        u32 ElementCount = 0;
        // Followed by ElementCount * CookedVertexLayoutElement.
    };

    // One vertex-buffer element in location order. Format is the underlying
    // Renderer::Format integer (cycle-avoidance rule at the top of this file).
    struct CookedVertexLayoutElement
    {
        u32 Format = 0; // underlying Renderer::Format
        char Name[ShaderNameCapacity] = {};
    };

    // Shader: reflected interface + SPIR-V. One cooked shader is one
    // SPIR-V module with one entry point, covering one shader stage — a
    // Material asset references a vertex-stage and a fragment-stage shader
    // as separate AssetIds. The blob is, in order:
    //   CookedShaderHeader
    //   CookedShaderInterfaceHeader
    //   CookedDescriptorBinding[BindingCount]
    //   CookedPushConstantBlock[PushConstantCount]
    //   SPIR-V bytes (SpirvBytes)
    // InterfaceBytes is the size of CookedShaderInterfaceHeader + the two
    // arrays (bindings + push constants) combined, so the loader can seek
    // straight to the SPIR-V. The referenced vertex layout is carried as
    // VertexLayoutAssetId in CookedShaderInterfaceHeader (0 = none).

    // Bindings/blocks (and the entry point below) are identified by name.

    struct CookedShaderHeader
    {
        // The SPIR-V module's OpEntryPoint name (Slang does not always emit
        // "main" — Shader::Create's ShaderBinaryInfo::EntryPoint must match
        // exactly, or pipeline creation fails validation).
        char EntryPoint[ShaderNameCapacity] = {};
        u32 InterfaceBytes = 0;
        u32 SpirvBytes = 0;
    };

    struct CookedShaderInterfaceHeader
    {
        u32 BindingCount = 0;
        u32 PushConstantCount = 0;
        u64 VertexLayoutAssetId = 0; // 0 = no vertex inputs (nullopt)
    };
    static_assert(sizeof(CookedShaderInterfaceHeader) == 16,
        "CookedShaderInterfaceHeader must be 16 bytes — guard against padding between the u32 fields and the u64");

    // One descriptor binding reflected from the shader, set >= 1 (set 0 is the
    // bindless registry — recognized and excluded by the importer).
    struct CookedDescriptorBinding
    {
        u32 Set = 0;
        u32 Binding = 0;
        u32 Type = 0;      // underlying Renderer::DescriptorType
        u32 Count = 1;
        u32 StageMask = 0; // underlying Renderer::ShaderStage (bitmask)
        char Name[ShaderNameCapacity] = {};
    };

    // One push-constant block (or field) reflected from the shader, validated
    // <=128B at cook time (Vulkan's guaranteed minimum push-constant block size).
    struct CookedPushConstantBlock
    {
        u32 Offset = 0;
        u32 Size = 0;
        u32 StageMask = 0; // underlying Renderer::ShaderStage (bitmask)
        char Name[ShaderNameCapacity] = {};
    };

    // Material: a thin bindless material — a vertex + fragment shader
    // (each an ordinary Shader asset, referenced by AssetId), and a packed
    // MaterialData parameter block described field-by-field. The blob is, in
    // order:
    //   CookedMaterialHeader
    //   CookedMaterialField[FieldCount]   — the reflected MaterialData layout
    //   packed param block (ParamBytes)   — the full MaterialData std140/std430
    //     image with scalar/vector params written and texture/sampler handle
    //     slots left zero (the loader patches them with runtime handles).
    // The two shader ids reference independent Shader pack entries (a forward
    // material needs one vertex- and one fragment-stage cooked shader; the
    // cooked-shader contract is one module / one entry point / one stage).
    //
    // The field table is reflected from the shader's MaterialData struct at cook
    // time, so it is self-describing: the loader patches handle fields by offset,
    // and name-based Material::SetTexture/SetParam resolve a field by Name. The
    // engine asserts ParamBytes == sizeof(its MaterialData mirror) on load — a
    // loud guard against shader/engine drift (cycle-avoidance house style).
    struct CookedMaterialHeader
    {
        u64 VertexShaderId = 0;   // AssetId of the vertex-stage Shader asset
        u64 FragmentShaderId = 0; // AssetId of the fragment-stage Shader asset
        u32 FieldCount = 0;
        u32 ParamBytes = 0;
    };

    // One reflected MaterialData field. Param fields (Kind 0) carry their value
    // pre-packed in the param block at Offset; handle fields (Kind 1/2) carry an
    // AssetId in TextureId that the loader resolves to a bindless handle and
    // writes as a u32 at Offset.
    struct CookedMaterialField
    {
        char Name[ShaderNameCapacity] = {};
        u32 Offset = 0; // byte offset within the MaterialData block
        u32 Size = 0;   // byte size of the field
        u32 Kind = 0;   // 0 = param value, 1 = sampled-image handle, 2 = sampler handle
        u64 TextureId = 0; // AssetId for Kinds 1/2; 0 for params
    };
}
