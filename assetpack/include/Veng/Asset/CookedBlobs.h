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
    /// @brief Maximum byte length (including nul terminator) for names in cooked blobs.
    ///
    /// Shared by shader bindings, blocks, attributes, and vertex-layout element names.
    /// Sized generously for GLSL/Slang identifiers; names are truncated at ShaderNameCapacity - 1.
    inline constexpr usize ShaderNameCapacity = 64;

    /// @brief Channel layout of a cooked texture's encoded blocks.
    ///
    /// Tells the runtime sampler how to interpret a texture's stored channels. Most codecs
    /// store their data verbatim (RGBA, two-channel RG, single-channel R), but the ASTC
    /// normal-map convention drops Z and stores only X/Y in two channels, so the sampler must
    /// reconstruct Z. The flag is distinct from the pixel format: an ASTC-XY normal is still an
    /// ASTC RGBA block, so the format ordinal alone cannot signal the convention.
    enum class CookedChannelLayout : u32
    {
        /// @brief The texture's channels are sampled as stored (the default for every codec).
        Direct = 0,
        /// @brief A tangent-space normal with X/Y stored and Z reconstructed in-shader.
        ///
        /// The ASTC normal convention: ASTC has no two-channel mode, so the cook stores X/Y in
        /// the texture's first two channels (unsigned, *2-1 unpack) and drops Z; the sampler
        /// reconstructs z = sqrt(1 - x^2 - y^2). BC5 carries the same XY-only normal data in a
        /// native two-channel codec and also reports this layout.
        NormalXY = 1,
    };

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
    ///
    /// The trailing ChannelLayout field carries the texture's channel convention; adding it set
    /// the texture header's on-disk layout, so a pack cooked before the field is size-mismatched
    /// and re-cooks (the loader rejects a blob shorter than this header).
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
        /// @brief Channel convention; underlying CookedChannelLayout integer (0 = Direct).
        u32 ChannelLayout = 0;
    };

    /// @brief The current mesh-format version.
    ///
    /// Bumped on any CookedMeshHeader/CookedMeshSocket layout change; the loader rejects a
    /// blob whose Version != this. The header is read by a fixed-offset memcpy, so a field
    /// added without a version bump is misread as garbage rather than tolerated.
    inline constexpr u32 CookedMeshVersion = 1u;

    /// @brief Cooked header for a mesh asset.
    ///
    /// The blob is, in order:
    ///   CookedMeshHeader
    ///   CookedVertexAttribute[AttributeCount]   — the interleaved layout
    ///   CookedSubMesh[SubMeshCount]             — draw ranges + material ids
    ///   CookedMeshSocket[SocketCount]           — named attachment points, sorted by name
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
        /// @brief Must equal CookedMeshVersion; the loader rejects mismatches.
        u32 Version = 0;
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
        /// @brief Number of CookedMeshSocket entries following the submesh table.
        u32 SocketCount = 0;
        /// @brief AssetId of the mesh's Skeleton, or 0 for a static (non-skinned) mesh.
        u64 SkeletonId = 0;
    };

    /// @brief One named attachment point on a cooked mesh, in mesh space.
    ///
    /// A socket is a named place on a model: the transform of an authored node that carries
    /// no geometry. Its rotation is an orientation, not decoration — local -Z is forward and
    /// local +Y is up, matching the glTF camera/node convention and the engine's y-up scene.
    /// Entries are sorted by Name so a lookup is a binary search and the blob is stable.
    ///
    /// The transform is stored as raw f32 arrays (xyzw for the quaternion) so assetpack gains
    /// no glm dependency; the engine loader reinterprets them into its glm types.
    struct CookedMeshSocket
    {
        /// @brief Nul-terminated socket name, at most ShaderNameCapacity - 1 bytes.
        char Name[ShaderNameCapacity] = {};
        /// @brief Mesh-space translation.
        f32 Position[3] = {};
        /// @brief Mesh-space rotation quaternion, xyzw.
        f32 Rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        /// @brief Mesh-space scale.
        f32 Scale[3] = {1.0f, 1.0f, 1.0f};
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
    inline constexpr u32 CookedMaterialVersion = 7u;

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
    /// 1 = PostProcess, 2 = Sky. The loader casts it to the engine enum, guarded by a VE_ASSERT
    /// on an out-of-range value. CullMode is the underlying integer of Renderer::CullMode,
    /// carried the same way.
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
        /// @brief Underlying Renderer::CullMode integer (0 = None, 1 = Front, 2 = Back).
        ///
        /// The material's authored face-culling mode; a material with no "cull" key cooks
        /// as Back (2), the engine's default for geometry pipelines.
        u32 CullMode = 2;
        /// @brief Authored translucent draw-order priority (the "sortPriority" key; default 0).
        ///
        /// Translucent draws sort back-to-front within ascending priority groups, so a
        /// higher-priority material draws after — over — every lower-priority one regardless
        /// of depth (an overlay plane, a reticle). Ignored outside the Translucent domain.
        i32 SortPriority = 0;
        /// @brief Number of CookedMaterialField entries following this header.
        u32 FieldCount = 0;
        /// @brief Byte size of the single parameter block; <= the per-material param stride.
        u32 BlockBytes = 0;
    };

    /// @brief One reflected material field within the single parameter block.
    ///
    /// Param fields (Kind 0) carry their value pre-packed at Offset. Handle fields (Kind 1/2)
    /// carry an AssetId in TextureId that the loader resolves to a bindless handle and writes
    /// as a u32 at Offset. A storage-buffer handle (Kind 3) is always runtime-bound (TextureId 0)
    /// — the game writes its bindless index per frame. Offset is within the one block.
    struct CookedMaterialField
    {
        /// @brief Nul-terminated field name, at most ShaderNameCapacity - 1 bytes.
        char Name[ShaderNameCapacity] = {};
        /// @brief Byte offset of the field within the parameter block.
        u32 Offset = 0;
        /// @brief Byte size of the field.
        u32 Size = 0;
        /// @brief Field kind: 0 = param value, 1 = sampled-image handle, 2 = sampler handle, 3 = storage-buffer handle.
        u32 Kind = 0;
        /// @brief AssetId for Kinds 1/2 (resolved to a bindless handle at load time); 0 for params and storage-buffer handles.
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

    /// @brief Geometry a cooked collision shape carries.
    ///
    /// Stored as the underlying integer of CookedCollisionShapeHeader::Mode; the engine mirrors
    /// it as Veng::CollisionGeometry. Integer values are stable — persisted in cooked blobs.
    enum class CookedCollisionGeometry : u32
    {
        /// @brief A point cloud whose convex hull is the shape; the cook stores hull vertices only.
        Convex = 0,
        /// @brief Indexed triangles, for non-simulated geometry: a static or kinematic body only.
        Mesh = 1,
    };

    /// @brief The current collision-shape-format version.
    ///
    /// Bumped on any CookedCollisionShapeHeader or payload layout change; the loader rejects a
    /// blob whose Version != this.
    inline constexpr u32 CookedCollisionShapeVersion = 1u;

    /// @brief Cooked header for a collision-shape asset.
    ///
    /// The blob is, in order:
    ///   CookedCollisionShapeHeader
    ///   f32 points[PointCount * 3]     — vertex positions, xyz, in the shape's local frame
    ///   u32 indices[IndexCount]        — triangle indices; empty under a Convex mode
    ///
    /// The geometry is **engine-owned and solver-neutral**: a point cloud for a convex hull, an
    /// indexed triangle soup for a mesh, both as raw f32/u32 so assetpack gains no math or
    /// physics dependency. The solver's own shape is built from it at load, so a solver version
    /// bump is a rebuild rather than a re-cook and no third-party binary format reaches the
    /// shipped asset layer. Convex hulling still happens once, offline: a Convex blob carries the
    /// hull's vertices, not the source model's.
    struct CookedCollisionShapeHeader
    {
        /// @brief Must equal CookedCollisionShapeVersion; the loader rejects mismatches.
        u32 Version = 0;
        /// @brief Which geometry follows; underlying CookedCollisionGeometry integer.
        u32 Mode = 0;
        /// @brief Number of xyz points following this header.
        u32 PointCount = 0;
        /// @brief Number of triangle indices following the points; a multiple of 3, or 0 for Convex.
        u32 IndexCount = 0;
    };

    /// @brief The current input-map-format version.
    ///
    /// Bumped on any CookedInputMapHeader layout change; the loader rejects a blob whose
    /// Version != this. The embedded reflection record evolves tolerantly within a fixed
    /// version — a new action or binding field does not require a bump.
    inline constexpr u32 CookedInputMapVersion = 1u;

    /// @brief Cooked header for an input-map asset.
    ///
    /// An input map (InputMappingContext) declares its actions (id + name + kind) and a set of
    /// raw-source → action bindings. Both lists ride the reflection serializer's name-keyed
    /// WriteFields record — assetpack treats it as opaque bytes, exactly as a prefab blob treats
    /// a component record, so this file gains no reflection dependency (cycle-avoidance rule at
    /// the top). The runtime loader ReadFields the record and builds the resolver-ready form.
    ///
    /// The blob is, in order:
    ///   CookedInputMapHeader
    ///   context record        — WriteFields record of { vector<InputAction>, vector<Binding> }
    struct CookedInputMapHeader
    {
        /// @brief Must equal CookedInputMapVersion; the loader rejects mismatches.
        u32 Version = 0;
        /// @brief Byte size of the reflection record following this header.
        u32 RecordBytes = 0;
    };

    /// @brief The current font-format version.
    ///
    /// Bumped on any CookedFontHeader/CookedGlyph/CookedKernPair layout change; the loader
    /// rejects a blob whose Version != this.
    inline constexpr u32 CookedFontVersion = 1u;

    /// @brief Cooked header for a font asset.
    ///
    /// A font is a multi-channel signed-distance-field (MSDF) glyph atlas plus the CPU metrics
    /// needed to lay out and draw text: a per-glyph table (codepoint, advance, plane bounds, and
    /// atlas rect) and a kerning table. The atlas is an ordinary RGBA8 image the runtime uploads
    /// and samples bindlessly; the three colour channels carry the MSDF, so it is never block
    /// compressed (a codec would corrupt the distance field). Every metric — advances, bounds,
    /// line metrics — is normalized to em units (the em is one unit), so a runtime pixel size is a
    /// single multiply.
    ///
    /// The blob is, in order:
    ///   CookedFontHeader
    ///   CookedGlyph[GlyphCount]
    ///   CookedKernPair[KerningCount]
    ///   atlas texels (AtlasWidth * AtlasHeight * 4 bytes, RGBA8, row-major top-to-bottom)
    struct CookedFontHeader
    {
        /// @brief Must equal CookedFontVersion; the loader rejects mismatches.
        u32 Version = 0;
        /// @brief MSDF atlas width in pixels.
        u32 AtlasWidth = 0;
        /// @brief MSDF atlas height in pixels.
        u32 AtlasHeight = 0;
        /// @brief Atlas pixel format; underlying Renderer::Format integer (RGBA8Unorm — the MSDF is a colour image).
        u32 AtlasFormat = 0;
        /// @brief Distance range, in atlas pixels, baked into the SDF; the shader divides screen-space distance by it.
        f32 DistanceRange = 0.0f;
        /// @brief Font units per em the metrics are normalized against (1.0 — the metrics are em-normalized).
        f32 EmSize = 0.0f;
        /// @brief Baseline-to-baseline line height, in em units.
        f32 LineHeight = 0.0f;
        /// @brief Ascender height above the baseline, in em units.
        f32 Ascender = 0.0f;
        /// @brief Descender depth below the baseline, in em units (negative below the baseline).
        f32 Descender = 0.0f;
        /// @brief Number of CookedGlyph entries following this header.
        u32 GlyphCount = 0;
        /// @brief Number of CookedKernPair entries following the glyph table.
        u32 KerningCount = 0;
    };

    /// @brief One glyph's metrics and atlas placement in a cooked font.
    ///
    /// Plane bounds are the glyph quad's offset and size relative to the pen origin on the
    /// baseline, in em units (left/bottom is the lower-left corner, so the quad spans
    /// [PlaneLeft, PlaneLeft + PlaneWidth] x [PlaneBottom, PlaneBottom + PlaneHeight]). Atlas
    /// bounds are the glyph's texel rect in the MSDF atlas, in pixels, top-left origin. A
    /// whitespace glyph has no geometry: its atlas rect and plane size are zero and only Advance
    /// is meaningful.
    struct CookedGlyph
    {
        /// @brief Unicode codepoint this glyph renders.
        u32 Codepoint = 0;
        /// @brief Horizontal advance from this glyph's origin to the next, in em units.
        f32 Advance = 0.0f;
        /// @brief Plane bounds left edge (quad offset from the pen origin), in em units.
        f32 PlaneLeft = 0.0f;
        /// @brief Plane bounds bottom edge (quad offset from the baseline), in em units.
        f32 PlaneBottom = 0.0f;
        /// @brief Plane bounds width (quad width), in em units.
        f32 PlaneWidth = 0.0f;
        /// @brief Plane bounds height (quad height), in em units.
        f32 PlaneHeight = 0.0f;
        /// @brief Atlas rect left edge, in atlas pixels.
        f32 AtlasLeft = 0.0f;
        /// @brief Atlas rect top edge, in atlas pixels.
        f32 AtlasTop = 0.0f;
        /// @brief Atlas rect width, in atlas pixels.
        f32 AtlasWidth = 0.0f;
        /// @brief Atlas rect height, in atlas pixels.
        f32 AtlasHeight = 0.0f;
    };

    /// @brief One kerning pair in a cooked font: the extra advance between an ordered glyph pair.
    ///
    /// Left and Right are Unicode codepoints; Advance is the kerning adjustment added to Left's
    /// advance when Right immediately follows it, in em units (usually negative — kerning tucks a
    /// pair closer). A font that kerns only through OpenType GPOS cooks an empty kerning table,
    /// since the cook reads the legacy `kern` table.
    struct CookedKernPair
    {
        /// @brief Codepoint of the left glyph in the ordered pair.
        u32 Left = 0;
        /// @brief Codepoint of the right glyph in the ordered pair.
        u32 Right = 0;
        /// @brief Kerning adjustment added to Left's advance when Right follows, in em units.
        f32 Advance = 0.0f;
    };

    /// @brief The current stylesheet-format version.
    ///
    /// Bumped on any CookedStyleSheetHeader/CookedStyleRule/CookedStyleProperty/
    /// CookedStyleAnimation/CookedStyleKeyframe/CookedStyleGradient/CookedStyleVariable layout change,
    /// and on any renumbering of the StyleProperty enumerators a CookedStyleProperty stores by ordinal;
    /// the loader rejects a blob whose Version != this. v2 added the @keyframes animation tables; v3
    /// added the gradient table and its baked ramp region; v4 widened a gradient's geometry to
    /// explicit endpoints (P0/P1) and an elliptical radial radius; v5 added the queryable variable
    /// table; v6 widened the gradient ramp from RGBA8 to RGBA16Sfloat half-float texels so a stop can
    /// hold an HDR (> 1) color; v7 renumbered the StyleProperty enumerators, replacing the single clip
    /// flag with the per-axis overflow properties and the scrollbar layout.
    inline constexpr u32 CookedStyleSheetVersion = 7u;

    /// @brief Maximum byte length (including nul terminator) for a selector's class/id/type name.
    ///
    /// A USS selector token (a class name, an id, an element type name) is truncated at
    /// StyleSelectorNameCapacity - 1 bytes.
    inline constexpr usize StyleSelectorNameCapacity = 64;

    /// @brief Cooked header for a stylesheet asset.
    ///
    /// A stylesheet is a flattened, resolved set of USS-like rules: each rule is a selector
    /// (an element type / class / id, optionally scoped to one pseudo-state) paired with the
    /// property declarations it sets. The cooker parses `*.vuss` selectors offline; the runtime
    /// never runs a selector engine — it matches an element's tags against these rules at load
    /// and cascades the survivors onto the element's resolved style. A stylesheet is a standalone,
    /// reusable asset: one stylesheet feeds many documents.
    ///
    /// The blob is, in order:
    ///   CookedStyleSheetHeader
    ///   CookedStyleRule[RuleCount]             — one entry per USS rule, in source order
    ///   CookedStyleProperty[PropertyCount]     — every rule's and keyframe's declarations
    ///   CookedStyleAnimation[AnimationCount]   — one entry per @keyframes clip, in source order
    ///   CookedStyleKeyframe[KeyframeCount]     — every clip's keyframes, contiguous per clip
    ///   CookedStyleGradient[GradientCount]     — one entry per `background-gradient`, in source order
    ///   CookedStyleVariable[VariableCount]     — the sheet's own queryable variables, in source order
    ///   u8[RampByteCount]                      — every gradient's baked N×1 RGBA16Sfloat ramp, contiguous
    ///
    /// A rule's declarations are the PropertyCount-slice [FirstProperty, FirstProperty + PropertyCount)
    /// of the property table; a keyframe's declarations slice the same table. Rules are stored in
    /// source order so a later rule of equal specificity wins the cascade, matching CSS
    /// source-order precedence. An `animation` declaration references a clip by its index in the
    /// animation table (resolved from the authored name at cook time — the name is not stored). A
    /// `background-gradient` declaration references a gradient by its index in the gradient table.
    /// The variable table carries only the sheet's own top-level `--` variables whose value resolves
    /// to a color or a single number, for runtime query; multi-token variables are cook-time-only and
    /// absent.
    struct CookedStyleSheetHeader
    {
        /// @brief Must equal CookedStyleSheetVersion; the loader rejects mismatches.
        u32 Version = 0;
        /// @brief Number of CookedStyleRule entries following this header.
        u32 RuleCount = 0;
        /// @brief Total number of CookedStyleProperty entries following the rule table.
        u32 PropertyCount = 0;
        /// @brief Number of CookedStyleAnimation entries following the property table.
        u32 AnimationCount = 0;
        /// @brief Total number of CookedStyleKeyframe entries following the animation table.
        u32 KeyframeCount = 0;
        /// @brief Number of CookedStyleGradient entries following the keyframe table.
        u32 GradientCount = 0;
        /// @brief Number of CookedStyleVariable entries following the gradient table.
        u32 VariableCount = 0;
        /// @brief Total bytes in the ramp region following the variable table.
        u32 RampByteCount = 0;
    };

    /// @brief One flattened USS rule: a selector, a pseudo-state, and its declaration range.
    ///
    /// The selector matches an element by its element type (Type), one class tag (Class), and/or
    /// one id (Id) — an empty field is a wildcard on that axis, so an empty Type/Class/Id matches
    /// any element (a `.class` rule sets only Class; a `Type.class#id` rule sets all three). State
    /// is the underlying Gui::ElementState integer the rule applies in (0 = the base state; a
    /// single set bit for a pseudo-state like `:hover`). The runtime cascades base rules onto the
    /// element's resolved style and keeps the state-scoped rules as variants keyed by State.
    struct CookedStyleRule
    {
        /// @brief Nul-terminated element type name the selector requires ("Button"), or empty for any type.
        char Type[StyleSelectorNameCapacity] = {};
        /// @brief Nul-terminated class tag the selector requires ("primary"), or empty for no class constraint.
        char Class[StyleSelectorNameCapacity] = {};
        /// @brief Nul-terminated id the selector requires ("start-button"), or empty for no id constraint.
        char Id[StyleSelectorNameCapacity] = {};
        /// @brief Underlying Gui::ElementState integer the rule applies in (0 = base, a single bit for a pseudo-state).
        u32 State = 0;
        /// @brief Index of this rule's first declaration in the property table.
        u32 FirstProperty = 0;
        /// @brief Number of declarations this rule sets.
        u32 PropertyCount = 0;
    };

    /// @brief One resolved style declaration within a cooked rule: a property id and its value.
    ///
    /// Property is the underlying Gui::StyleProperty integer naming which Style field the
    /// declaration sets. The value rides a uniform payload the loader interprets by property:
    /// Unit carries a Length's LengthKind ordinal or an enum property's enumerator ordinal;
    /// Values holds the numeric payload (a Length's value in [0], a vec4/CornerRadii/Insets in
    /// [0..3], a single f32 in [0]); Handle carries a font property's AssetId (else 0). Colors are
    /// resolved to linear straight-alpha floats at cook time (authored as sRGB `#rrggbbaa` decoded to
    /// linear, or as unclamped-linear `rgb()`/`rgba()`), matching the draw-list contract.
    struct CookedStyleProperty
    {
        /// @brief Underlying Gui::StyleProperty integer naming the Style field this declaration sets.
        u32 Property = 0;
        /// @brief A Length's LengthKind ordinal or an enum property's enumerator ordinal; 0 otherwise.
        u32 Unit = 0;
        /// @brief Numeric payload: a Length value ([0]), a vec4/CornerRadii/Insets ([0..3]), or a scalar ([0]).
        f32 Values[4] = {};
        /// @brief AssetId for a font property (resolved as a load-time dependency); 0 otherwise.
        u64 Handle = 0;
    };

    /// @brief One cooked @keyframes clip: its keyframe range in the keyframe table.
    struct CookedStyleAnimation
    {
        /// @brief Index of the clip's first keyframe in the keyframe table.
        u32 FirstKeyframe = 0;
        /// @brief Number of keyframes in the clip.
        u32 KeyframeCount = 0;
    };

    /// @brief One cooked keyframe: its normalized offset and its declaration range.
    struct CookedStyleKeyframe
    {
        /// @brief The keyframe's position along the clip, normalized to [0, 1].
        f32 Offset = 0.0f;
        /// @brief Index of the keyframe's first declaration in the property table.
        u32 FirstProperty = 0;
        /// @brief Number of declarations the keyframe holds.
        u32 PropertyCount = 0;
    };

    /// @brief One cooked gradient fill: its shape, packed box-space geometry, and its baked ramp.
    ///
    /// A `background-gradient` declaration references one of these by its index in the gradient
    /// table. The multi-stop color is baked at cook time into an N×1 RGBA16Sfloat ramp (linear
    /// straight-alpha half-floats, HDR-capable) stored in the ramp region at
    /// [RampOffset, RampOffset + RampTexels * 8).
    /// Kind is the Gui::GradientKind ordinal; the geometry is in the element's normalized box space
    /// and interpreted per Kind (Linear: P0 start, P1 end; Radial: P0 center, P1 (x, y) radii;
    /// Conic: P0 center, AngleOffset start turn).
    struct CookedStyleGradient
    {
        /// @brief The Gui::GradientKind ordinal (0 Linear, 1 Radial, 2 Conic).
        u32 Kind = 0;
        /// @brief Linear start point / radial + conic center, in normalized box space.
        f32 P0[2] = {};
        /// @brief Linear end point / radial (x, y) radii, in normalized box space.
        f32 P1[2] = {};
        /// @brief Conic start turn in [0, 1); unused otherwise.
        f32 AngleOffset = 0.0f;
        /// @brief Byte offset of this gradient's ramp in the ramp region.
        u32 RampOffset = 0;
        /// @brief Number of RGBA16Sfloat texels in the ramp (its byte length is RampTexels * 8).
        u32 RampTexels = 0;
    };

    /// @brief One queryable stylesheet variable: a name, its value kind, and the resolved payload.
    ///
    /// Only a sheet's own top-level `--` variable whose full value resolves to a color or a single
    /// number carries an entry; a multi-token variable is cook-time-only and never stored. Name is
    /// the authored variable name without the leading `--` (`--accent` is stored as "accent"). Kind
    /// selects how Payload is read: a color (0) fills all four channels (linear straight-alpha,
    /// matching the draw-list contract); a scalar (1) fills only Payload[0]. A stylesheet's `@use`d
    /// variables are never stored here — a consumer queries the theme sheet that owns them.
    struct CookedStyleVariable
    {
        /// @brief Nul-terminated variable name without the leading `--`, at most StyleSelectorNameCapacity - 1 bytes.
        char Name[StyleSelectorNameCapacity] = {};
        /// @brief Value kind: 0 = color (Payload xyzw), 1 = scalar (Payload[0]).
        u32 Kind = 0;
        /// @brief Resolved value: a linear straight-alpha color (all four), or a scalar in [0].
        f32 Payload[4] = {};
    };

    /// @brief The current UI-document-format version.
    ///
    /// Bumped on any CookedUIDocumentHeader/CookedUIElement/inline-property/blob-region layout
    /// change, and on any renumbering of the StyleProperty enumerators the inline-property table
    /// stores by ordinal; the loader rejects a blob whose Version != this. v3 renumbered them for
    /// the per-axis overflow properties.
    inline constexpr u32 CookedUIDocumentVersion = 3u;

    /// @brief Cooked header for a UI-document asset.
    ///
    /// A UI document is a binary element tree: a flat, pre-order array of elements (each carrying
    /// its kind, id, class tags, text, `{binding}` expression strings, handler names, and inline
    /// style overrides), the stylesheets it references, and its font/texture dependencies. The
    /// cooker parses the `*.vui.xml` markup offline; the runtime materializes an independent live
    /// element tree from this immutable recipe (the Prefab model — two instances over one blob).
    ///
    /// The blob is, in order:
    ///   CookedUIDocumentHeader
    ///   u64[StyleSheetCount]                 — referenced StyleSheet AssetIds (load-time dependencies)
    ///   CookedUIElement[ElementCount]        — the element tree, pre-order
    ///   CookedUIStringSpan[ClassCount]       — every element's class tags, contiguous per element
    ///   CookedUIBinding[BindingCount]        — every element's bindings, contiguous per element
    ///   CookedUIHandler[HandlerCount]        — every element's handlers, contiguous per element
    ///   CookedStyleProperty[InlinePropertyCount] — every element's inline-style declarations, contiguous per element
    ///   string region (StringBytes)          — UTF-8 strings the elements index by byte offset
    ///
    /// Each element addresses its strings (id, text, class list, binding expressions, handler names)
    /// by a (offset, length) pair into the string region, so a name appears once and an element is a
    /// fixed-size record. The tree is pre-order with an explicit child count per element, so the
    /// loader reconstructs the hierarchy in one linear pass.
    struct CookedUIDocumentHeader
    {
        /// @brief Must equal CookedUIDocumentVersion; the loader rejects mismatches.
        u32 Version = 0;
        /// @brief Number of referenced StyleSheet AssetIds following this header.
        u32 StyleSheetCount = 0;
        /// @brief Number of CookedUIElement entries following the stylesheet id array.
        u32 ElementCount = 0;
        /// @brief Total number of CookedUIStringSpan class-tag entries following the element array.
        u32 ClassCount = 0;
        /// @brief Total number of CookedUIBinding entries following the class-tag table.
        u32 BindingCount = 0;
        /// @brief Total number of CookedUIHandler entries following the binding table.
        u32 HandlerCount = 0;
        /// @brief Total number of CookedStyleProperty inline-style entries following the handler table.
        u32 InlinePropertyCount = 0;
        /// @brief Byte size of the trailing string region.
        u32 StringBytes = 0;
    };

    /// @brief A byte range into a cooked UI document's string region.
    ///
    /// Offset is a byte offset into the string region; Length is the byte length of the UTF-8
    /// string there. A zero-length span is the empty string. Every string an element carries (id,
    /// text, one class tag, one binding key/expression, one handler name) is addressed this way, so
    /// the element record stays fixed-size and a repeated name is deduplicated in the region.
    struct CookedUIStringSpan
    {
        /// @brief Byte offset of the string within the document's string region.
        u32 Offset = 0;
        /// @brief Byte length of the string; 0 for the empty string.
        u32 Length = 0;
    };

    /// @brief One element in a cooked UI document's pre-order element tree.
    ///
    /// Kind is the underlying Gui::ElementKind integer. Id and Text are single string spans; the
    /// class tags, binding expressions, and handler names are contiguous runs in their respective
    /// side tables (Classes/Bindings/Handlers), each element naming its run's first index and count.
    /// FirstInlineProperty/InlinePropertyCount slice the inline-style property table. ChildCount is
    /// the number of immediately-following elements (recursively) that are this element's direct
    /// children — the pre-order layout lets the loader rebuild the hierarchy in one pass.
    struct CookedUIElement
    {
        /// @brief Underlying Gui::ElementKind integer this element instantiates.
        u32 Kind = 0;
        /// @brief The element's id string span (empty when untagged).
        CookedUIStringSpan Id;
        /// @brief The Text element's content string span (empty for non-text kinds and untexted elements).
        CookedUIStringSpan Text;
        /// @brief Index of this element's first class tag in the class-span table.
        u32 FirstClass = 0;
        /// @brief Number of class tags this element carries.
        u32 ClassCount = 0;
        /// @brief Index of this element's first binding in the binding table.
        u32 FirstBinding = 0;
        /// @brief Number of `{binding}` expressions this element carries.
        u32 BindingCount = 0;
        /// @brief Index of this element's first handler in the handler table.
        u32 FirstHandler = 0;
        /// @brief Number of named event handlers this element carries.
        u32 HandlerCount = 0;
        /// @brief Index of this element's first inline-style declaration in the inline-property table.
        u32 FirstInlineProperty = 0;
        /// @brief Number of inline-style declarations this element sets.
        u32 InlinePropertyCount = 0;
        /// @brief Number of direct children of this element (they follow in pre-order).
        u32 ChildCount = 0;
        /// @brief An Image element's source texture AssetId (a load-time texture dependency); 0 when none.
        u64 Src = 0;
        /// @brief An Image element's tint, linear straight-alpha RGBA; opaque white (1,1,1,1) by default.
        f32 Tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        /// @brief An Image element's UV sub-rect {minX, minY, sizeX, sizeY}; the whole texture (0,0,1,1) by default.
        f32 Uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    };

    /// @brief One binding on a cooked UI element: the target property name and its expression.
    ///
    /// Property is the bound attribute name (e.g. "value"); Expression is the unresolved
    /// `{obj.field}` expression string with the braces stripped (e.g. "player.health"). Both are
    /// string spans into the document's string region. Binding expressions are stored unresolved;
    /// the runtime resolves them against a bound context.
    struct CookedUIBinding
    {
        /// @brief The bound attribute name string span (e.g. "value").
        CookedUIStringSpan Property;
        /// @brief The unresolved binding expression string span (braces stripped, e.g. "player.health").
        CookedUIStringSpan Expression;
    };

    /// @brief One named event handler on a cooked UI element: the event name and the handler name.
    ///
    /// Event is the event attribute name (e.g. "onClick"); Handler is the C++ handler name the
    /// attribute names (e.g. "OpenMenu"). Both are string spans into the document's string region.
    /// Handler names are stored unresolved; the runtime resolves them against a bound context.
    struct CookedUIHandler
    {
        /// @brief The event attribute name string span (e.g. "onClick").
        CookedUIStringSpan Event;
        /// @brief The unresolved handler name string span (e.g. "OpenMenu").
        CookedUIStringSpan Handler;
    };

    /// @brief How a data table's key index is ordered and binary-searched.
    ///
    /// Not a cell-type vocabulary: a cell's type is a reflection TypeId, and this names only the
    /// ordering the separate key index is built under. A key column's reflected type resolves to
    /// exactly one of these two at cook time.
    enum class CookedTableKeyKind : u32
    {
        /// @brief Keys are integers, widened to i64 and ordered numerically.
        Integer = 0,
        /// @brief Keys are UTF-8 strings in the key heap, ordered byte-wise lexicographically.
        String = 1,
    };

    /// @brief Column offset sentinel meaning the column is not arithmetic-addressable.
    ///
    /// A column's cell sits at a constant byte offset in every row only while every preceding
    /// column is fixed-size; the first variable-size column and everything after it carries this
    /// value, and reaching such a cell requires walking the row's preceding cells.
    inline constexpr u32 CookedTableColumnOffsetUnresolved = 0xFFFFFFFFu;

    /// @brief The current table-schema-format version.
    ///
    /// Bumped on any CookedTableSchemaHeader/CookedTableColumn layout change; the loader rejects
    /// a blob whose Version != this.
    inline constexpr u32 CookedTableSchemaVersion = 2u;

    /// @brief Cooked header for a table-schema asset.
    ///
    /// The blob is, in order:
    ///   CookedTableSchemaHeader
    ///   CookedTableColumn[ColumnCount]     — in authored declaration order
    ///
    /// A column's type is a reflection TypeId stored as a raw u64; assetpack never interprets it,
    /// which is what keeps this header free of any reflection dependency. The cook resolves each
    /// id against a TypeRegistry to size and encode a cell.
    struct CookedTableSchemaHeader
    {
        /// @brief Must equal CookedTableSchemaVersion; the loader rejects mismatches.
        u32 Version = 0;
        /// @brief Number of CookedTableColumn entries following this header.
        u32 ColumnCount = 0;
        /// @brief Index of the key column within the column array.
        u32 KeyColumn = 0;
        /// @brief The key index's ordering; underlying CookedTableKeyKind integer.
        u32 KeyKind = 0;
        /// @brief Byte size of one row record; meaningful only when FixedStride is 1.
        u32 RowStride = 0;
        /// @brief 1 when every column's type encodes to a constant byte count, 0 otherwise.
        ///
        /// A fixed-stride table omits the row directory and addresses rows arithmetically. It is
        /// a property of the cooked blob, not of the format contract: the runtime accessor API is
        /// identical either way.
        u32 FixedStride = 0;
    };

    /// @brief One column of a cooked table schema.
    struct CookedTableColumn
    {
        /// @brief Nul-terminated column name, at most ShaderNameCapacity - 1 bytes.
        char Name[ShaderNameCapacity] = {};
        /// @brief The column's reflection TypeId, stored as a raw u64.
        u64 Type = 0;
        /// @brief Byte offset of this column's cell within a row, or CookedTableColumnOffsetUnresolved.
        u32 Offset = 0;
        /// @brief Explicit pad keeping the entry's size deterministic.
        u32 Pad = 0;
    };

    /// @brief A string cell: a byte range within a data table's key heap.
    struct CookedTableStringSpan
    {
        /// @brief Byte offset of the first character within the key heap.
        u32 Offset = 0;
        /// @brief Length in bytes; the heap stores no terminator.
        u32 Length = 0;
    };

    /// @brief One entry of a data table's sorted key index.
    ///
    /// Sorted ascending on the key's logical order — numeric for an Integer key kind, byte-wise
    /// lexicographic over the key heap for a String key kind — so a lookup is a binary search
    /// with no allocation. Keys are unique; the table cook rejects a duplicate. The index is a
    /// separate structure from the row directory: the two answer different questions.
    struct CookedTableKey
    {
        /// @brief The key value under an Integer key kind; 0 under a String key kind.
        i64 IntKey = 0;
        /// @brief The key-heap span under a String key kind; zero-length under an Integer key kind.
        CookedTableStringSpan StringKey;
        /// @brief Index of the row this key addresses.
        u32 RowIndex = 0;
        /// @brief Explicit pad keeping the entry at a deterministic 24 bytes.
        u32 Pad = 0;
    };

    /// @brief The current data-table-format version.
    ///
    /// Bumped on any CookedDataTableHeader/CookedTableKey/row layout change; the loader rejects
    /// a blob whose Version != this.
    inline constexpr u32 CookedDataTableVersion = 2u;

    /// @brief Cooked header for a data-table asset.
    ///
    /// The blob is, in order:
    ///   CookedDataTableHeader
    ///   CookedTableKey[RowCount]           — the sorted key index
    ///   u32 RowOffsets[RowCount]           — the row directory; present only when FixedStride is 0
    ///   row records                        — RowBytes bytes of reflected cell encodings
    ///   key heap                           — KeyHeapBytes bytes backing the String key spans
    ///
    /// Row offsets are u32, so the row region is capped just under 4 GiB; the cook fails loudly
    /// rather than truncate, since a truncated offset would load clean and address garbage.
    ///
    /// KeyKind is stored here rather than read from the schema so a key lookup is self-describing:
    /// the table resolves a row without consulting its schema handle, which matters because the
    /// schema is an ordinary streamed dependency.
    struct CookedDataTableHeader
    {
        /// @brief AssetId of the TableSchema these rows were cooked against.
        u64 SchemaId = 0;
        /// @brief Must equal CookedDataTableVersion; the loader rejects mismatches.
        u32 Version = 0;
        /// @brief Number of rows, of key-index entries, and of row-directory entries.
        u32 RowCount = 0;
        /// @brief 1 when the rows are a fixed stride and the directory is omitted; must match the schema.
        u32 FixedStride = 0;
        /// @brief Byte size of one row record; meaningful only when FixedStride is 1.
        u32 RowStride = 0;
        /// @brief Byte size of the row region.
        u32 RowBytes = 0;
        /// @brief Byte size of the trailing key heap.
        u32 KeyHeapBytes = 0;
        /// @brief The key index's ordering; underlying CookedTableKeyKind integer.
        u32 KeyKind = 0;
        /// @brief Explicit pad keeping the header at a deterministic 40 bytes.
        u32 Pad = 0;
    };
}
