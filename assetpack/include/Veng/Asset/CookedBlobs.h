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
    /// types per the cycle-avoidance rule above.
    ///
    /// The header is followed by MipCount mip levels, tightly packed largest-first (level 0 =
    /// the full-resolution image, then each successive halving down to 1x1). For an
    /// uncompressed format a level's byte size derives purely from its dimensions —
    /// max(1, Width >> i) * max(1, Height >> i) * bytes-per-pixel(Format) — so the blob carries
    /// no per-level offset table; the loader walks the levels arithmetically. A single-mip
    /// texture (MipCount == 1) is the degenerate one-level case of this layout.
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
    /// validate it against the engine's canonical (or skinned) VertexBufferLayout (a loud
    /// Corrupt error on mismatch, not silent UB). The descriptor is stored self-describingly
    /// so a layout change is a format-version bump, not a silent reinterpretation.
    ///
    /// A non-zero SkeletonId marks a skinned mesh: its vertices carry the skinned layout
    /// (canonical attributes plus BoneIndices/BoneWeights) and the loader resolves the
    /// referenced Skeleton as a load-time dependency.
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
        /// @brief AssetId of the mesh's Skeleton, or 0 for a static (non-skinned) mesh.
        u64 SkeletonId = 0;
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

    /// @brief The current material-instance-format version.
    ///
    /// Bumped on any layout change; the loader rejects a blob whose Version != this.
    inline constexpr u32 CookedMaterialInstanceVersion = 1u;

    /// @brief Cooked header for a material-instance asset.
    ///
    /// A material instance is a sparse parameter override over a parent Material: it owns no
    /// shader or pipeline, only its own per-material SSBO slot seeded from the parent's default
    /// block and patched by its overrides. The parent supplies the pipeline, the reflected field
    /// schema, and the default param bytes; the instance overrides exactly the exposed fields the
    /// parent reports, by name.
    ///
    /// The blob is, in order:
    ///   CookedMaterialInstanceHeader
    ///   CookedMaterialInstanceOverride[OverrideCount]
    ///   override value region (ValueRegionBytes) — the param overrides' raw bytes, concatenated
    ///
    /// A param override (Kind 0) references ValueOffset/ValueSize bytes in the value region, which
    /// the loader copies into the seeded block at the parent field's reflected offset. A texture
    /// override (Kind 1) carries a TextureId the loader resolves to a bindless index and patches at
    /// the parent field's offset; its ValueSize is 0.
    struct CookedMaterialInstanceHeader
    {
        /// @brief AssetId of the parent Material; resolved as a load-time dependency.
        u64 ParentId = 0;
        /// @brief Must equal CookedMaterialInstanceVersion; the loader rejects mismatches.
        u32 Version = 0;
        /// @brief Number of CookedMaterialInstanceOverride entries following this header.
        u32 OverrideCount = 0;
        /// @brief Byte size of the trailing override value region.
        u32 ValueRegionBytes = 0;
    };

    /// @brief One field override in a cooked material instance, matched against a parent field by name.
    ///
    /// Kind 0 (param) carries its replacement bytes at [ValueOffset, ValueOffset + ValueSize) in
    /// the value region. Kind 1 (texture) carries the override texture's AssetId in TextureId; the
    /// loader resolves it to a bindless index and writes it at the parent field's offset, with
    /// ValueSize 0.
    struct CookedMaterialInstanceOverride
    {
        /// @brief Nul-terminated parent-field name, at most ShaderNameCapacity - 1 bytes.
        char Name[ShaderNameCapacity] = {};
        /// @brief Override kind: 0 = param value, 1 = texture handle.
        u32 Kind = 0;
        /// @brief Byte offset of this override's value within the value region (param overrides only).
        u32 ValueOffset = 0;
        /// @brief Byte size of this override's value in the value region; 0 for a texture override.
        u32 ValueSize = 0;
        /// @brief Override texture's AssetId (texture overrides only); 0 for a param override.
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

    /// @brief The current skeleton-format version.
    ///
    /// Bumped on any CookedSkeletonHeader/CookedBone layout change; the loader rejects a
    /// blob whose Version != this.
    inline constexpr u32 CookedSkeletonVersion = 1u;

    /// @brief Cooked header for a skeleton asset.
    ///
    /// A skeleton is a flat array of bones in topological order (every bone precedes its
    /// children), each carrying its parent index, inverse-bind matrix, and local bind-pose
    /// transform. Animations and the skinning palette index this same bone order. The
    /// runtime computes each bone's skinning matrix as
    /// GlobalInverse * modelBone(bone) * InverseBind(bone).
    ///
    /// The blob is, in order:
    ///   CookedSkeletonHeader
    ///   CookedBone[BoneCount]
    struct CookedSkeletonHeader
    {
        /// @brief Must equal CookedSkeletonVersion; the loader rejects mismatches.
        u32 Version = 0;
        /// @brief Number of CookedBone entries following this header.
        u32 BoneCount = 0;
        /// @brief Inverse of the scene root transform, column-major mat4; folded into the skin formula.
        f32 GlobalInverse[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    };

    /// @brief One bone in a cooked skeleton, in topological (parent-before-child) order.
    ///
    /// All matrices/transforms are stored as raw f32 arrays (column-major for the matrix,
    /// xyzw for the quaternion) so assetpack gains no glm dependency; the engine loader
    /// reinterprets them into its glm types.
    struct CookedBone
    {
        /// @brief Index of the parent bone in the bone array, or -1 for a root.
        i32 Parent = -1;
        /// @brief Nul-terminated bone name, at most ShaderNameCapacity - 1 bytes.
        char Name[ShaderNameCapacity] = {};
        /// @brief Inverse bind-pose matrix (mesh space → bone space), column-major mat4.
        f32 InverseBind[16] = {};
        /// @brief Local bind-pose translation (parent space).
        f32 LocalPosition[3] = {};
        /// @brief Local bind-pose rotation quaternion, xyzw (parent space).
        f32 LocalRotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        /// @brief Local bind-pose scale (parent space).
        f32 LocalScale[3] = {1.0f, 1.0f, 1.0f};
    };

    /// @brief The current animation-format version.
    ///
    /// Bumped on any CookedAnimationHeader/CookedAnimChannel/key layout change; the loader
    /// rejects a blob whose Version != this.
    inline constexpr u32 CookedAnimationVersion = 1u;

    /// @brief Cooked header for an animation asset.
    ///
    /// An animation is a set of per-bone keyframe tracks (position/rotation/scale) sampled
    /// against a skeleton's bone order. Times are in seconds; a bone with no track for a
    /// component holds its skeleton bind-pose value.
    ///
    /// The blob is, in order:
    ///   CookedAnimationHeader
    ///   CookedAnimChannel[ChannelCount]
    ///   key region                            — CookedVec3Key / CookedQuatKey runs the
    ///                                            channels reference by byte offset (KeyRegionBytes)
    struct CookedAnimationHeader
    {
        /// @brief Must equal CookedAnimationVersion; the loader rejects mismatches.
        u32 Version = 0;
        /// @brief Number of CookedAnimChannel entries following this header.
        u32 ChannelCount = 0;
        /// @brief Byte size of the trailing key region.
        u32 KeyRegionBytes = 0;
        /// @brief Total animation duration in seconds.
        f32 Duration = 0.0f;
    };

    /// @brief One bone's animation track: position/rotation/scale key runs by byte offset.
    ///
    /// Offsets are byte offsets into the animation's key region (the bytes following the
    /// channel array). Position and scale keys are CookedVec3Key; rotation keys are
    /// CookedQuatKey. A zero count means the bone holds its bind-pose value for that channel.
    struct CookedAnimChannel
    {
        /// @brief Target bone index in the skeleton's bone array.
        u32 BoneIndex = 0;
        /// @brief Number of position keys.
        u32 PositionKeyCount = 0;
        /// @brief Byte offset of the position keys within the key region.
        u32 PositionKeyOffset = 0;
        /// @brief Number of rotation keys.
        u32 RotationKeyCount = 0;
        /// @brief Byte offset of the rotation keys within the key region.
        u32 RotationKeyOffset = 0;
        /// @brief Number of scale keys.
        u32 ScaleKeyCount = 0;
        /// @brief Byte offset of the scale keys within the key region.
        u32 ScaleKeyOffset = 0;
    };

    /// @brief One timed vec3 key (position or scale) in an animation track.
    struct CookedVec3Key
    {
        /// @brief Key time in seconds.
        f32 Time = 0.0f;
        /// @brief Keyed value (xyz).
        f32 Value[3] = {};
    };

    /// @brief One timed quaternion key (rotation) in an animation track.
    struct CookedQuatKey
    {
        /// @brief Key time in seconds.
        f32 Time = 0.0f;
        /// @brief Keyed rotation quaternion, xyzw.
        f32 Value[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };

    /// @brief Current CookedEnvironmentHeader version; the loader rejects a mismatch.
    inline constexpr u32 CookedEnvironmentVersion = 1u;

    /// @brief Cooked header for an environment-map asset.
    ///
    /// The blob is the header followed by Width * Height raw texels in the given Format
    /// (an equirectangular HDR panorama, row-major top-to-bottom). The engine generates the
    /// IBL cubemap, irradiance, and prefilter maps from this panorama at load. The sampler is
    /// fixed by the loader (linear, clamp-to-edge), so no sampler fields are stored.
    struct CookedEnvironmentHeader
    {
        /// @brief Must equal CookedEnvironmentVersion; the loader rejects mismatches.
        u32 Version = 0;
        /// @brief Pixel format; underlying Renderer::Format integer (RGBA16Sfloat).
        u32 Format = 0;
        /// @brief Panorama width in pixels.
        u32 Width = 0;
        /// @brief Panorama height in pixels.
        u32 Height = 0;
    };
}
