#pragma once

#include <Veng/Asset/Types.h>

// Per-type cooked-blob header layouts (planset-5 plan 02). Each header is
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
// These headers are defined here as the shared contract; their production
// (cooker) and consumption (engine loaders) land per-type in plans 06-09, each
// filling in the reserved fields (mips, layout descriptor, interface bytes,
// param/binding tables) as that type arrives.

namespace Veng
{
    // Texture: stb-decoded, single mip v1 (mip table reserved for later).
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

    // Mesh: interleaved vertices + indices in a declared layout (plan 07). The
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
    // k_ShaderNameCapacity - 1 — generous for GLSL/Slang identifiers. Shared
    // by shader bindings/blocks/attributes and vertex-layout element names.
    inline constexpr usize k_ShaderNameCapacity = 64;

    // VertexLayout: a named list of vertex-buffer elements that shaders and
    // pipelines reference by AssetId (plan 08b). The blob is, in order:
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
        char Name[k_ShaderNameCapacity] = {};
    };

    // Shader: reflected interface + SPIR-V (plan 08). One cooked shader is one
    // SPIR-V module with one entry point, covering one shader stage — a
    // material (plan 09) references a vertex-stage and a fragment-stage shader
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
        char EntryPoint[k_ShaderNameCapacity] = {};
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
    // bindless registry, plan 05 — recognized and excluded by the importer).
    struct CookedDescriptorBinding
    {
        u32 Set = 0;
        u32 Binding = 0;
        u32 Type = 0;      // underlying Renderer::DescriptorType
        u32 Count = 1;
        u32 StageMask = 0; // underlying Renderer::ShaderStage (bitmask)
        char Name[k_ShaderNameCapacity] = {};
    };

    // One push-constant block (or field) reflected from the shader, validated
    // <=128B at cook time (planset-2/01's guaranteed minimum block size).
    struct CookedPushConstantBlock
    {
        u32 Offset = 0;
        u32 Size = 0;
        u32 StageMask = 0; // underlying Renderer::ShaderStage (bitmask)
        char Name[k_ShaderNameCapacity] = {};
    };

    // Material: shader ref + params + texture bindings (defined in plan 09).
    // Followed by TextureBindingCount texture-binding entries, then ParamBytes
    // bytes of material parameter data.
    struct CookedMaterialHeader
    {
        u64 ShaderId = 0; // AssetId
        u32 TextureBindingCount = 0;
        u32 ParamBytes = 0;
    };
}
