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

    // Mesh: interleaved vertices + indices in a declared layout. Followed by a
    // vertex layout descriptor, the submesh table (CookedSubMesh[SubMeshCount]),
    // then the vertex and index buffers (plan 07 fills in the layout
    // descriptor's exact form).
    struct CookedMeshHeader
    {
        u32 VertexStride = 0;
        u32 VertexCount = 0;
        u32 IndexCount = 0;
        u32 IndexType = 0; // underlying Renderer::IndexType
        u32 SubMeshCount = 0;
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

    // Shader: reflected interface + SPIR-V (ShaderInterface serialization
    // defined in plan 08). Followed by InterfaceBytes bytes of serialized
    // ShaderInterface, then SpirvBytes bytes of SPIR-V.
    struct CookedShaderHeader
    {
        u32 InterfaceBytes = 0;
        u32 SpirvBytes = 0;
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
