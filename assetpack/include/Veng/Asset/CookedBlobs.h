#pragma once

#include <Veng/Asset/Types.h>

// Per-type cooked-blob header layouts. Each header is
// followed in the blob by the type's payload, as noted per struct.
//
// The cycle-avoidance rule (load-bearing): enum-typed fields below (pixel
// Format, index Type, shader stages, ...) are stored as their *underlying
// integer type* — assetpack deliberately does not include the engine's
// renderer vocabulary header. The engine loader casts to the corresponding
// Veng::Renderer enum guarded by a static_assert/VE_ASSERT (loud one-line fix
// on drift, per house style). This keeps assetpack standalone and the
// cooker buildable without the engine.
//
// These headers are the shared contract between the cooker (production) and the
// engine loaders (consumption).

namespace Veng
{
    /// @brief Cooked header for a texture asset.
    ///
    /// Sampler fields mirror Veng::Renderer::SamplerInfo, stored as underlying integer/float
    /// types per the cycle-avoidance rule above. The header is followed by
    /// Width * Height * bytes-per-pixel(Format) raw pixel bytes.
    struct CookedTextureHeader
    {
        /// @brief Pixel format; underlying Renderer::Format integer.
        u32 Format = 0;
        /// @brief Texture width in pixels.
        u32 Width = 0;
        /// @brief Texture height in pixels.
        u32 Height = 0;
        /// @brief Number of mip levels.
        u32 MipCount = 1;

        /// @brief Minification filter; underlying Renderer::Filter integer.
        u32 MinFilter = 0;
        /// @brief Magnification filter; underlying Renderer::Filter integer.
        u32 MagFilter = 0;
        /// @brief Mipmap filter mode; underlying Renderer::MipmapMode integer.
        u32 MipmapMode = 0;
        /// @brief U-axis address mode; underlying Renderer::AddressMode integer.
        u32 AddressModeU = 0;
        /// @brief V-axis address mode; underlying Renderer::AddressMode integer.
        u32 AddressModeV = 0;
        /// @brief W-axis address mode; underlying Renderer::AddressMode integer.
        u32 AddressModeW = 0;
        /// @brief Whether anisotropic filtering is enabled (stored as u32 bool).
        u32 AnisotropyEnabled = 0;
        /// @brief Maximum anisotropy level when anisotropic filtering is enabled.
        f32 MaxAnisotropy = 1.0f;
    };

    /// @brief Cooked header for a mesh asset.
    ///
    /// The blob is, in order:
    ///   CookedMeshHeader
    ///   CookedVertexAttribute[AttributeCount]   — the interleaved layout
    ///   CookedSubMesh[SubMeshCount]             — draw ranges + material ids
    ///   vertex bytes  (VertexCount * VertexStride)
    ///   index bytes   (IndexCount * (IndexType == U16 ? 2 : 4))
    ///
    /// The attribute descriptor records the on-disk interleaved format so the loader can
    /// validate it against the engine's canonical VertexBufferLayout (a loud Corrupt error
    /// on mismatch, not silent UB). The descriptor is stored self-describingly so a layout
    /// change is a format-version bump, not a silent reinterpretation.
    struct CookedMeshHeader
    {
        /// @brief Byte stride per vertex.
        u32 VertexStride = 0;
        /// @brief Number of vertices in the vertex buffer.
        u32 VertexCount = 0;
        /// @brief Number of indices in the index buffer.
        u32 IndexCount = 0;
        /// @brief Index element type; underlying Renderer::IndexType integer.
        u32 IndexType = 0;
        /// @brief Number of CookedSubMesh entries following the attribute table.
        u32 SubMeshCount = 0;
        /// @brief Number of CookedVertexAttribute entries following this header.
        u32 AttributeCount = 0;
    };

    /// @brief One interleaved vertex attribute, in layout order.
    ///
    /// Offset is the byte offset within a vertex (redundant with the running stride, but
    /// stored so validation is a direct field-by-field compare).
    struct CookedVertexAttribute
    {
        /// @brief Attribute format; underlying Renderer::Format integer.
        u32 Format = 0;
        /// @brief Byte offset of this attribute within a vertex.
        u32 Offset = 0;
    };

    /// @brief One draw range within a cooked mesh's index buffer.
    ///
    /// MaterialId is an AssetId forward reference resolved at load time.
    struct CookedSubMesh
    {
        /// @brief First index in the index buffer for this sub-mesh.
        u32 IndexOffset = 0;
        /// @brief Number of indices in this sub-mesh's draw call.
        u32 IndexCount = 0;
        /// @brief AssetId of the material this sub-mesh was authored against.
        u64 MaterialId = 0;
    };

    /// @brief Maximum byte length (including nul terminator) for names in cooked blobs.
    ///
    /// Shared by shader bindings, blocks, attributes, and vertex-layout element names.
    /// Sized generously for GLSL/Slang identifiers; names are truncated at ShaderNameCapacity - 1.
    inline constexpr usize ShaderNameCapacity = 64;

    /// @brief Cooked header for a vertex layout asset.
    ///
    /// The blob is, in order:
    ///   CookedVertexLayoutHeader
    ///   CookedVertexLayoutElement[ElementCount]
    struct CookedVertexLayoutHeader
    {
        /// @brief Number of CookedVertexLayoutElement entries following this header.
        u32 ElementCount = 0;
    };

    /// @brief One vertex-buffer element in location order.
    ///
    /// Format is the underlying Renderer::Format integer (cycle-avoidance rule at the top of this file).
    struct CookedVertexLayoutElement
    {
        /// @brief Element format; underlying Renderer::Format integer.
        u32 Format = 0;
        /// @brief Nul-terminated element name, at most ShaderNameCapacity - 1 bytes.
        char Name[ShaderNameCapacity] = {};
    };

    /// @brief Cooked header for a shader asset.
    ///
    /// One cooked shader is one SPIR-V module with one entry point, covering one shader stage.
    /// A Material asset references a vertex-stage and a fragment-stage shader as separate AssetIds.
    /// The blob is, in order:
    ///   CookedShaderHeader
    ///   CookedShaderInterfaceHeader
    ///   CookedDescriptorBinding[BindingCount]
    ///   CookedPushConstantBlock[PushConstantCount]
    ///   SPIR-V bytes (SpirvBytes)
    ///
    /// InterfaceBytes is the size of CookedShaderInterfaceHeader plus the two arrays combined,
    /// so the loader can seek straight to the SPIR-V. The referenced vertex layout is carried
    /// as VertexLayoutAssetId in CookedShaderInterfaceHeader (0 = none).
    /// Bindings, blocks, and the entry point are identified by name.
    struct CookedShaderHeader
    {
        /// @brief The SPIR-V module's OpEntryPoint name.
        ///
        /// Slang does not always emit "main" — Shader::Create's ShaderBinaryInfo::EntryPoint
        /// must match exactly, or pipeline creation fails validation.
        char EntryPoint[ShaderNameCapacity] = {};
        /// @brief Byte size of the reflected interface region (CookedShaderInterfaceHeader + binding + push-constant arrays).
        u32 InterfaceBytes = 0;
        /// @brief Byte size of the SPIR-V module that follows the interface region.
        u32 SpirvBytes = 0;
    };

    /// @brief Reflected interface counts and optional vertex-layout reference for a shader.
    struct CookedShaderInterfaceHeader
    {
        /// @brief Number of CookedDescriptorBinding entries.
        u32 BindingCount = 0;
        /// @brief Number of CookedPushConstantBlock entries.
        u32 PushConstantCount = 0;
        /// @brief AssetId of the referenced vertex layout, or 0 if the shader has no vertex inputs.
        u64 VertexLayoutAssetId = 0;
    };
    static_assert(sizeof(CookedShaderInterfaceHeader) == 16,
                  "CookedShaderInterfaceHeader must be 16 bytes — guard against padding between "
                  "the u32 fields and the u64");

    /// @brief One descriptor binding reflected from the shader.
    ///
    /// Set is >= 1; set 0 is the bindless registry and is excluded by the importer.
    struct CookedDescriptorBinding
    {
        /// @brief Descriptor set index (always >= 1).
        u32 Set = 0;
        /// @brief Binding index within the set.
        u32 Binding = 0;
        /// @brief Descriptor type; underlying Renderer::DescriptorType integer.
        u32 Type = 0;
        /// @brief Descriptor array count.
        u32 Count = 1;
        /// @brief Shader stage bitmask; underlying Renderer::ShaderStage integer.
        u32 StageMask = 0;
        /// @brief Nul-terminated binding name, at most ShaderNameCapacity - 1 bytes.
        char Name[ShaderNameCapacity] = {};
    };

    /// @brief One push-constant block (or field) reflected from the shader.
    ///
    /// Validated to be <= 128 bytes at cook time (Vulkan's guaranteed minimum push-constant block size).
    struct CookedPushConstantBlock
    {
        /// @brief Byte offset of the block within the push-constant range.
        u32 Offset = 0;
        /// @brief Byte size of the block.
        u32 Size = 0;
        /// @brief Shader stage bitmask; underlying Renderer::ShaderStage integer.
        u32 StageMask = 0;
        /// @brief Nul-terminated block name, at most ShaderNameCapacity - 1 bytes.
        char Name[ShaderNameCapacity] = {};
    };

    /// @brief The current material-format version.
    ///
    /// Bumped on any layout change; the loader rejects a blob whose Version != this.
    inline constexpr u32 CookedMaterialVersion = 4u;

    /// @brief Cooked header for a material asset.
    ///
    /// A material pairs a vertex and fragment shader (each an ordinary Shader asset referenced by
    /// AssetId) with a single parameter block holding both bindless handle slots and authored
    /// scalar/vector uniforms, byte-addressed at each field's reflected offset.
    ///
    /// The blob is, in order:
    ///   CookedMaterialHeader
    ///   CookedMaterialField[FieldCount]   — one entry per declared field
    ///   param block (BlockBytes)          — handle slots left zero (the loader patches them with
    ///                                       runtime bindless indices) and authored params written
    ///                                       at their reflected offsets
    ///
    /// The field table is reflected from the shader at cook time and is self-describing: a field's
    /// Kind tells the loader whether the u32 at its offset is a bindless handle slot to patch
    /// (Kind 1/2) or an authored value to keep (Kind 0). The loader patches handle fields by offset;
    /// Material::SetTexture/SetParam resolve a field by Name.
    ///
    /// The engine asserts Version == CookedMaterialVersion (a stale blob is a loud reject) and
    /// BlockBytes <= the per-material param stride.
    ///
    /// Domain is the underlying integer of Veng::MaterialDomain (cycle-avoidance rule above):
    /// 0 = Surface (the default — a material with no "domain" key cooks as Surface),
    /// 1 = PostProcess. The loader casts it to the engine enum, guarded by a VE_ASSERT on an
    /// out-of-range value.
    struct CookedMaterialHeader
    {
        /// @brief AssetId of the vertex-stage Shader asset.
        u64 VertexShaderId = 0;
        /// @brief AssetId of the fragment-stage Shader asset.
        u64 FragmentShaderId = 0;
        /// @brief Must equal CookedMaterialVersion; the loader rejects mismatches.
        u32 Version = 0;
        /// @brief Underlying MaterialDomain integer (0 = Surface, 1 = PostProcess).
        u32 Domain = 0;
        /// @brief Number of CookedMaterialField entries following this header.
        u32 FieldCount = 0;
        /// @brief Byte size of the single parameter block; <= the per-material param stride.
        u32 BlockBytes = 0;
    };

    /// @brief One reflected material field within the single parameter block.
    ///
    /// Param fields (Kind 0) carry their value pre-packed at Offset. Handle fields (Kind 1/2)
    /// carry an AssetId in TextureId that the loader resolves to a bindless handle and writes
    /// as a u32 at Offset. Offset is within the one block.
    struct CookedMaterialField
    {
        /// @brief Nul-terminated field name, at most ShaderNameCapacity - 1 bytes.
        char Name[ShaderNameCapacity] = {};
        /// @brief Byte offset of the field within the parameter block.
        u32 Offset = 0;
        /// @brief Byte size of the field.
        u32 Size = 0;
        /// @brief Field kind: 0 = param value, 1 = sampled-image handle, 2 = sampler handle.
        u32 Kind = 0;
        /// @brief AssetId for Kinds 1/2 (resolved to a bindless handle at load time); 0 for params.
        u64 TextureId = 0;
    };

    /// @brief The current prefab-format version.
    ///
    /// Bumped on any layout change; the loader rejects a blob whose Version != this.
    inline constexpr u32 CookedPrefabVersion = 1u;

    /// @brief Cooked header for a prefab asset.
    ///
    /// A prefab is a tree of entities, each carrying components keyed by their stable TypeId, with
    /// each component's field values stored as the reflection serializer's name-keyed record.
    /// assetpack treats the records as opaque bytes — the engine's PrefabLoader interprets them
    /// through the TypeRegistry, so this file gains no reflection/engine dependency
    /// (cycle-avoidance rule at the top).
    ///
    /// The blob is, in order:
    ///   CookedPrefabHeader
    ///   CookedPrefabEntity[EntityCount]
    ///   CookedPrefabComponent[ComponentCount]   — each entity's components are a contiguous run
    ///   record blob (RecordBytes)               — the WriteFields records, concatenated
    ///
    /// A Reference (Entity) inside a record stores the prefab-local entity index (its position in
    /// CookedPrefabEntity[]) in the Entity's Index slot, with Generation written as 0; the loader
    /// remaps it to the spawned handle. The reserved index Entity::Null.Index (~0u) is a null
    /// reference the loader leaves null — never a valid prefab-local index, so null and an
    /// intra-prefab reference never collide.
    struct CookedPrefabHeader
    {
        /// @brief Must equal CookedPrefabVersion; the loader rejects mismatches.
        u32 Version = 0;
        /// @brief Number of CookedPrefabEntity entries.
        u32 EntityCount = 0;
        /// @brief Total number of CookedPrefabComponent entries across all entities.
        u32 ComponentCount = 0;
        /// @brief Byte size of the trailing record blob.
        u32 RecordBytes = 0;
    };

    /// @brief One entity in a cooked prefab, referencing a contiguous run of its components.
    struct CookedPrefabEntity
    {
        /// @brief Index of this entity's first component in the component table.
        u32 FirstComponent = 0;
        /// @brief Number of components belonging to this entity.
        u32 ComponentCount = 0;
    };

    /// @brief One component entry in the cooked prefab's component table.
    struct CookedPrefabComponent
    {
        /// @brief The component's stable type id, matching the engine's TypeRegistry.
        u64 TypeId = 0;
        /// @brief Byte offset of this component's record within the record blob.
        u32 RecordOffset = 0;
        /// @brief Byte size of this component's record.
        u32 RecordSize = 0;
    };

    /// @brief The current level-format version.
    ///
    /// Bumped on any CookedLevelHeader layout change; the loader rejects a blob whose
    /// Version != this. The two embedded reflection records evolve tolerantly within a
    /// fixed version — a new game-mode or render-settings field does not require a bump.
    inline constexpr u32 CookedLevelVersion = 1u;

    /// @brief Cooked header for a level asset.
    ///
    /// A level wraps a world prefab by AssetId and carries the level-scoped wiring: the
    /// ordered set of active systems, the game-mode config, and a render-settings subset.
    /// The game-mode and render config each ride the reflection serializer's name-keyed
    /// WriteFields record — assetpack treats both as opaque bytes, exactly as the prefab
    /// blob treats a component record, so this file gains no reflection dependency
    /// (cycle-avoidance rule at the top). The system ids select catalog entries the
    /// engine's SystemRegistry resolves at load.
    ///
    /// The blob is, in order:
    ///   CookedLevelHeader
    ///   u64[SystemCount]      — the ordered active SystemId set
    ///   game-mode record      — WriteFields record of the game-mode config (GameModeRecordBytes)
    ///   render record         — WriteFields record of the render settings (RenderRecordBytes)
    struct CookedLevelHeader
    {
        /// @brief Must equal CookedLevelVersion; the loader rejects mismatches.
        u32 Version = 0;
        /// @brief AssetId of the world prefab this level spawns; resolved as a load-time dependency.
        u64 WorldPrefabId = 0;
        /// @brief Number of u64 SystemId entries following this header, in run order.
        u32 SystemCount = 0;
        /// @brief Byte size of the game-mode config record following the system-id array.
        u32 GameModeRecordBytes = 0;
        /// @brief Byte size of the render-settings record following the game-mode record.
        u32 RenderRecordBytes = 0;
    };
}
