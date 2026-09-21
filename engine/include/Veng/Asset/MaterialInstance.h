#pragma once

#include <span>
#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetBuild.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/Material.h>
#include <Veng/Renderer/BindlessRegistry.h>

namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;
}

namespace Veng
{
    class Texture;

    /// @brief One field override applied over a parent material's default block, matched by name.
    struct MaterialOverride
    {
        /// @brief The parent field name this override targets.
        string Name;
        /// @brief Replacement bytes for a param override (written at the parent field's offset); empty for a texture override.
        vector<std::byte> Value;
        /// @brief Override texture for a texture-handle override; empty for a param override.
        AssetHandle<Texture> Texture;
    };

    /// @brief Construction parameters for MaterialInstance, assembled by MaterialInstanceLoader.
    struct MaterialInstanceInfo
    {
        /// @brief Debug name for the instance.
        string Name;
        /// @brief Render context used for the bindless slot allocation.
        Renderer::Context* Context = nullptr;
        /// @brief The parent material this instance overrides; kept resident for the instance's lifetime.
        AssetHandle<Material> Parent;
        /// @brief Sparse field overrides applied over the parent's default block.
        vector<MaterialOverride> Overrides;
    };

    /// @brief A cheap parameter override over a parent Material — its own SSBO slot, no shader.
    ///
    /// A MaterialInstance owns one parameter-block entry in the bindless registry's per-material
    /// buffer, seeded from the parent's default block and patched by its overrides; it keeps its
    /// parent (and any override textures) resident. It borrows the parent's pipeline, layout,
    /// schema, and domain — binding the parent's pipeline and pushing **its own** selector. A
    /// runtime-built instance plus per-frame SetParam is the MID (Material Instance Dynamic): the
    /// ring-buffered SetParam/SetTexture writes are stall-free, landing in the current
    /// frame-in-flight region. **A Set* copies the field's bytes, not the block** — so writing a
    /// field list every frame costs the fields, and the replication into the other regions costs
    /// one span covering them.
    ///
    /// One AssetId names one asset of one type: a parent Material's id and its cooked
    /// default-instance id are distinct assets, and a MaterialInstance request for a bare Material
    /// id is a WrongType, never a synthesized default.
    ///
    /// **A block is per frame-in-flight, not per view.** The registry rings the arena by
    /// frames-in-flight alone (see BindlessRegistry::MaterialArenaBytes), so a value written between
    /// two Viewport::Render calls of one frame is the value *both* viewports' draws read at submit —
    /// the last writer wins for every view. A consumer needing a per-view value therefore needs a
    /// per-view instance: build one per renderer with AssetManager::BuildSync over the shared
    /// instance's parent and CopyParamsFrom the shared one, then write the per-view value into the
    /// copy.
    class MaterialInstance
    {
    public:
        ~MaterialInstance();

        MaterialInstance(const MaterialInstance&) = delete;
        MaterialInstance& operator=(const MaterialInstance&) = delete;

        /// @brief Binds the parent's pipeline and pushes this instance's index as the per-draw selector.
        ///
        /// Set 0 (the bindless registry) must already be bound for the frame. Issue mesh draws
        /// after this call. A PostProcess parent has no owned pipeline; only the selector is pushed.
        /// A Surface parent pushes nothing — the geometry pass reads the instance's index from the
        /// per-draw DrawData SSBO.
        void Bind(Renderer::CommandBuffer& cmd) const;

        /// @brief Binds the parent's skinned g-buffer pipeline for a skinned draw.
        ///
        /// The skinned sibling of Bind: it binds the three-set skinned pipeline (set 0 bindless,
        /// set 1 DrawData, set 2 the skinning palette) so the geometry pass's palette bind at set 2
        /// is valid. A Surface material reads its selector from the DrawData SSBO, so nothing is
        /// pushed. Only meaningful for a Surface parent that built a skinned variant.
        /// @pre The parent has a skinned pipeline (EnsureSkinnedPipeline has run).
        void BindSkinned(Renderer::CommandBuffer& cmd) const;

        /// @brief Ensures the parent's skinned g-buffer pipeline is built (render-thread, lazy, idempotent).
        ///
        /// Delegates to the parent Material. The renderer calls this before a skinned draw so
        /// BindSkinned has a pipeline to bind. A no-op for a non-Surface parent and after the first
        /// build.
        /// @param assets  Asset manager used to load the skinned vertex shader and its layout.
        void EnsureSkinnedPipeline(AssetManager& assets) const;

        /// @brief Creates a runtime copy sharing this instance's parent, with its own SSBO slot.
        ///
        /// The copy keeps the same parent (and this instance's override textures) resident and
        /// seeds its parameter block from this instance's **current** block — so it reproduces the
        /// live appearance, overrides and any Set* writes since Finalize included — then registers
        /// its own bindless per-material slot. Mutating the copy
        /// (SetParam/SetTexture/SetTextureHandle/…) therefore affects only the copy, never this
        /// instance or any other entity drawing a shared material. Render-thread only (it allocates
        /// a bindless slot); the caller owns the returned Ref and wraps it with AssetManager::Adopt
        /// to draw with it.
        /// @param name  Debug name for the copy.
        /// @return The runtime copy, finalized and ready to draw.
        /// @pre This instance is finalized (registered).
        [[nodiscard]] Ref<MaterialInstance> Clone(std::string_view name) const;

        /// @brief Overwrites this instance's parameter block with @p source's current one.
        ///
        /// Copies the live block — the parent's defaults, @p source's authored overrides and every
        /// Set* write since — and takes over @p source's resident override textures, then uploads.
        /// This is how a per-view instance tracks the shared instance it stands in for: re-copying
        /// whenever GetRevision() moves keeps an override, an editor tweak or a game write on the
        /// shared instance reaching the draw, which a one-time seed would not.
        /// @param source The instance to copy from.
        /// @pre Both instances are finalized and share one parent Material.
        void CopyParamsFrom(const MaterialInstance& source);

        /// @brief Returns the frame-folded material selector (GetCurrentFrameBase() + block offset).
        ///
        /// The value the shader uses to index the ring-buffered per-material parameter block. A
        /// Surface draw's geometry pass writes this into each per-draw DrawData record instead of
        /// pushing it; it changes per frame-in-flight, so read it at record time.
        [[nodiscard]] u32 GetMaterialSelector() const;

        /// @brief Resolves a field name to a handle into the parent's schema.
        ///
        /// Delegates to the parent Material, which owns the table — so the handle is valid for
        /// every instance of that parent, and a pool of instances resolves its names once at
        /// construction rather than once per instance per write.
        /// @param name The field name to resolve.
        /// @return A handle naming the field, or an invalid handle when the schema has no such name.
        [[nodiscard]] MaterialFieldHandle Field(std::string_view name) const
        {
            return m_Parent.Get()->Field(name);
        }

        /// @brief Sets the texture for a named handle field and rewrites the SSBO entry in place.
        void SetTexture(std::string_view name, AssetHandle<Texture> texture);

        /// @brief Sets the texture for a resolved handle field and rewrites the SSBO entry in place.
        ///
        /// Writes the texture's bindless index into the field and, when the field carries one, its
        /// sampler index into the paired `<name>Sampler` field; the texture is kept resident on the
        /// instance. The handle must name a TextureHandle field of this instance's parent.
        /// @param field   Handle of the TextureHandle field to write.
        /// @param texture The texture to bind, kept resident.
        void SetTexture(MaterialFieldHandle field, AssetHandle<Texture> texture);

        /// @brief Sets a vec4 parameter by field name, rewriting the SSBO entry in place.
        void SetParam(std::string_view name, const vec4& value);

        /// @brief Sets a vec4 parameter by resolved handle, rewriting the SSBO entry in place.
        ///
        /// Writes the field's reflected Size bytes, so a vec2 or vec3 field takes the leading
        /// components and nothing spills into the block that follows. An array field is a fatal
        /// here — it is written a whole table at a time by SetParamArray, or an element at a time
        /// by the indexed overload.
        /// @param field Handle of the Param field to write.
        /// @param value The value to write.
        void SetParam(MaterialFieldHandle field, const vec4& value);

        /// @brief Sets one element of a vector-array parameter by resolved handle.
        ///
        /// Writes the field's ElementStride bytes at `Offset + index * ElementStride`, so a
        /// `float3[]` element takes the leading three components and its neighbours are untouched.
        /// An index at or past the field's ElementCount is a fatal rather than a write into the
        /// element after it.
        /// @param field Handle of the Param field to write.
        /// @param index Element to write, below the field's ElementCount.
        /// @param value The value to write.
        void SetParam(MaterialFieldHandle field, u32 index, const vec4& value);

        /// @brief Sets one element of a scalar-array parameter by resolved handle.
        ///
        /// The scalar form of the indexed setter, with the same bounds rule.
        /// @param field Handle of the Param field to write.
        /// @param index Element to write, below the field's ElementCount.
        /// @param value The value to write.
        void SetParam(MaterialFieldHandle field, u32 index, f32 value);

        /// @brief Writes a whole vector-array parameter in one ranged write.
        ///
        /// The array is one logical parameter, so a table of N entries costs one write of its
        /// N * ElementStride bytes rather than N writes. Each value contributes the field's
        /// ElementStride leading bytes, so a `float3[]` takes xyz of every vec4. A span whose
        /// length is not the field's ElementCount is a fatal rather than a partial write — a table
        /// filled short is the mistake this catches.
        /// @param field  Handle of the Param field to write.
        /// @param values Exactly ElementCount values, in element order.
        void SetParamArray(MaterialFieldHandle field, std::span<const vec4> values);

        /// @brief Writes a whole scalar-array parameter in one ranged write.
        ///
        /// The scalar form of the whole-array write, with the same length rule. The field's
        /// elements must be scalars.
        /// @param field  Handle of the Param field to write.
        /// @param values Exactly ElementCount values, in element order.
        void SetParamArray(MaterialFieldHandle field, std::span<const f32> values);

        /// @brief Sets a scalar float parameter by field name.
        ///
        /// Writes only the field's reflected Size bytes, never smearing a vec4 over adjacent fields.
        void SetParam(std::string_view name, f32 value);

        /// @brief Sets a scalar float parameter by resolved handle.
        ///
        /// Writes only the field's reflected Size bytes, never smearing a vec4 over adjacent
        /// fields. An array field is a fatal here, as for the vec4 overload.
        /// @param field Handle of the Param field to write.
        /// @param value The value to write.
        void SetParam(MaterialFieldHandle field, f32 value);

        /// @brief Writes a raw bindless texture index into a TextureHandle field by name.
        ///
        /// For runtime-bound inputs (a renderer-owned ImageView the instance does not own as a
        /// cooked Texture asset). Does not keep any asset resident. The write lands in the
        /// ring-buffered block's current frame region — cheap and frame-safe.
        void SetTextureHandle(std::string_view name, Renderer::TextureHandle handle);

        /// @brief Writes a raw bindless texture index into a TextureHandle field by resolved handle.
        ///
        /// The per-frame form of the runtime-bound path: a pass or a surface that rebinds the same
        /// slot every frame resolves the field once and writes the index thereafter.
        /// @param field  Handle of the TextureHandle field to write.
        /// @param handle The bindless texture handle to bind.
        void SetTextureHandle(MaterialFieldHandle field, Renderer::TextureHandle handle);

        /// @brief Writes a raw bindless sampler index into a SamplerHandle field by name.
        ///
        /// Same semantics as SetTextureHandle but targets a SamplerHandle field.
        void SetSamplerHandle(std::string_view name, Renderer::SamplerHandle handle);

        /// @brief Writes a raw bindless sampler index into a SamplerHandle field by resolved handle.
        ///
        /// Same semantics as SetTextureHandle but targets a SamplerHandle field.
        /// @param field  Handle of the SamplerHandle field to write.
        /// @param handle The bindless sampler handle to bind.
        void SetSamplerHandle(MaterialFieldHandle field, Renderer::SamplerHandle handle);

        /// @brief Writes a bindless byte-address storage-buffer index into a StorageBufferHandle field by name.
        ///
        /// Experimental, opt-in. Binds a game-supplied storage buffer to a material by handle: the
        /// game creates a storage Buffer, uploads its data, registers it with BindlessRegistry to get
        /// a StorageBufferHandle, and sets it here; the shader reads it typed through the set-0
        /// g_Buffers[] array (g_Buffers[params.Handle].Load<T>(off)). The field carries no cooked
        /// default — it is runtime-bound. Keeps no asset resident; the caller owns the buffer's
        /// lifetime and its bindless registration. The write lands in the ring-buffered block's
        /// current frame region, so it is cheap and frame-safe.
        /// @param name   The StorageBufferHandle field to write.
        /// @param handle The bindless storage-buffer handle to bind.
        void SetStorageBufferHandle(std::string_view name, Renderer::StorageBufferHandle handle);

        /// @brief Writes a bindless byte-address storage-buffer index into a StorageBufferHandle field by resolved handle.
        ///
        /// Same semantics as the name-taking overload, with the field resolved once.
        /// @param field  Handle of the StorageBufferHandle field to write.
        /// @param handle The bindless storage-buffer handle to bind.
        void SetStorageBufferHandle(MaterialFieldHandle field,
                                    Renderer::StorageBufferHandle handle);

        /// @brief Writes a raw bindless volume (3D sampled-image) index into a VolumeHandle field by name.
        ///
        /// The 3D counterpart of SetTextureHandle: binds a runtime-registered volume to a material.
        /// The game builds a VolumeField (or any Type3D ImageView), registers it with
        /// BindlessRegistry::RegisterVolume to get a VolumeHandle, and sets it here; the shader
        /// samples it through the typed g_Volumes[] array. The field carries no cooked default — it
        /// is runtime-bound. Keeps no asset resident; the caller owns the resource and its bindless
        /// registration. The write lands in the ring-buffered block's current frame region, so it
        /// is cheap and frame-safe.
        /// @param name   The VolumeHandle field to write.
        /// @param handle The bindless volume handle to bind.
        void SetVolumeHandle(std::string_view name, Renderer::VolumeHandle handle);

        /// @brief Writes a raw bindless volume (3D sampled-image) index into a VolumeHandle field by resolved handle.
        ///
        /// Same semantics as the name-taking overload, with the field resolved once.
        /// @param field  Handle of the VolumeHandle field to write.
        /// @param handle The bindless volume handle to bind.
        void SetVolumeHandle(MaterialFieldHandle field, Renderer::VolumeHandle handle);

        /// @brief Returns the instance's byte offset within one region of the material arena.
        ///
        /// The frame base is *not* folded in — this is where the block sits in every region, which
        /// is what the registry's own surfaces are keyed by. GetMaterialSelector() is the value a
        /// draw pushes.
        [[nodiscard]] u32 GetBlockOffset() const { return m_Handle.Offset; }

        /// @brief Returns the instance's current parameter block, as last written.
        ///
        /// The CPU-side cache the ranged writers keep in step with the arena, so it is what the
        /// shader will read: the parent's defaults, the instance's authored overrides and every
        /// Set* since. The instance sibling of Material::GetDefaultBlock, and the read a consumer
        /// verifying its own writes needs — a field's bytes are at its MaterialField Offset, an
        /// array element i at Offset + i * ElementStride.
        /// @return The block's bytes; empty until the instance is finalized.
        [[nodiscard]] std::span<const std::byte> GetBlock() const { return m_Block; }

        /// @brief Returns a revision that bumps on every parameter/handle write.
        ///
        /// A monotonic counter incremented whenever the instance's parameter block is rewritten
        /// (any Set*, and the initial Finalize). A consumer that caches a derived result from the
        /// instance's params — e.g. a baked sky cube — compares this against the revision it last
        /// derived against to detect an in-place param change, since the instance pointer is
        /// unchanged when only its contents are mutated.
        [[nodiscard]] u32 GetRevision() const { return m_Revision; }

        /// @brief Returns the instance's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns the parent material handle.
        [[nodiscard]] const AssetHandle<Material>& GetParent() const { return m_Parent; }

        /// @brief Returns the parent's domain (Surface or PostProcess).
        [[nodiscard]] MaterialDomain GetDomain() const { return m_Parent.Get()->GetDomain(); }

        /// @brief Returns the parent's graphics pipeline, or null for a PostProcess parent.
        [[nodiscard]] const Ref<Renderer::GraphicsPipeline>& GetPipeline() const
        {
            return m_Parent.Get()->GetPipeline();
        }

        /// @brief Returns the parent's skinned g-buffer pipeline, or null when it has none.
        [[nodiscard]] const Ref<Renderer::GraphicsPipeline>& GetSkinnedPipeline() const
        {
            return m_Parent.Get()->GetSkinnedPipeline();
        }

        /// @brief Returns the parent's skinned pipeline layout (three sets), or null when it has none.
        [[nodiscard]] Ref<Renderer::PipelineLayout> GetSkinnedPipelineLayout() const
        {
            return m_Parent.Get()->GetSkinnedPipelineLayout();
        }

        /// @brief Returns the parent's reflected pipeline layout.
        [[nodiscard]] const Ref<Renderer::PipelineLayout>& GetPipelineLayout() const
        {
            return m_Parent.Get()->GetPipelineLayout();
        }

        /// @brief Returns the parent's vertex shader module.
        [[nodiscard]] const Ref<Renderer::ShaderModule>& GetVertexModule() const
        {
            return m_Parent.Get()->GetVertexModule();
        }

        /// @brief Returns the parent's fragment shader module.
        [[nodiscard]] const Ref<Renderer::ShaderModule>& GetFragmentModule() const
        {
            return m_Parent.Get()->GetFragmentModule();
        }

        /// @brief Returns the parent's per-draw selector push offset.
        [[nodiscard]] u32 GetSelectorOffset() const { return m_Parent.Get()->GetSelectorOffset(); }

        /// @brief Returns the parent's reflected field schema.
        [[nodiscard]] std::span<const MaterialField> GetFields() const
        {
            return m_Parent.Get()->GetFields();
        }

        /// @brief Returns the parent's resident texture dependencies (the defaults the instance inherits).
        ///
        /// A caller drawing with this instance must PrepareForAccess(tex->GetView(), Sample) before
        /// the draw, exactly as for a parent material's textures; the instance's override textures
        /// are returned by GetOverrideTextures().
        [[nodiscard]] std::span<const AssetHandle<Texture>> GetTextures() const
        {
            return m_Parent.Get()->GetTextures();
        }

        /// @brief Returns the instance's resident texture overrides (sampled in place of the parent defaults).
        [[nodiscard]] std::span<const AssetHandle<Texture>> GetOverrideTextures() const
        {
            return m_Textures;
        }

    private:
        friend class MaterialInstanceLoader;
        friend Task<Detail::BuiltAsset<MaterialInstance>>
        Detail::SubmitAssetBuild(Renderer::Context& context, TaskSystem& tasks,
                                 MaterialInstanceInfo data);
        friend Ref<MaterialInstance> Detail::BuildAssetSync(Renderer::Context& context,
                                                            const MaterialInstanceInfo& data);

        /// @brief Constructs an unfinalized instance holding its parent handle and overrides.
        ///
        /// The worker-legal construction step: it touches nothing behind the handles, so the parent
        /// may still be pending here (a cold async load resolves it as a dependency). The result is
        /// Finalize()d on the render thread (block seed + override patch + bindless slot
        /// allocation) before use, and the parent must be resident by then.
        static Ref<MaterialInstance> Prepare(const MaterialInstanceInfo& info)
        {
            return Ref<MaterialInstance>(new MaterialInstance(info));
        }

        /// @brief Seeds the block from the parent, applies the overrides, allocates the per-material SSBO slot, and uploads.
        ///
        /// Runs on the render thread (the parent and the override textures are resident by now, so
        /// the parent's patched default block and the textures' bindless indices resolve). A param
        /// override is copied at its parent field's offset; a texture override patches the field's
        /// handle slot.
        /// @pre The parent material is resident.
        void Finalize();

        explicit MaterialInstance(const MaterialInstanceInfo& info);

        [[nodiscard]] const MaterialField& ResolveField(MaterialFieldHandle field,
                                                        std::string_view caller) const;
        /// @brief Resolves a handle that must name a Param field.
        [[nodiscard]] const MaterialField& ResolveParamField(MaterialFieldHandle field,
                                                             std::string_view caller) const;
        [[nodiscard]] MaterialFieldHandle RequireField(std::string_view name,
                                                       std::string_view caller) const;
        /// @brief Uploads the whole cached block and bumps the revision.
        void UploadParams() const;

        /// @brief Uploads one byte range of the cached block and bumps the revision.
        /// @param offset Byte offset of the range within the block.
        /// @param bytes  Length of the range.
        void UploadParams(u32 offset, u32 bytes) const;

        Renderer::Context& m_Context;
        string m_Name;
        AssetHandle<Material> m_Parent;
        vector<MaterialOverride> m_Overrides;
        vector<AssetHandle<Texture>> m_Textures;

        vector<std::byte> m_Block;
        Renderer::MaterialHandle m_Handle;
        bool m_Registered = false;
        /// @brief Bumped on every parameter-block write so a caching consumer detects an in-place
        /// content change; mutable because UploadParams (the write chokepoint) is const.
        mutable u32 m_Revision = 0;
    };

    /// @brief AssetTypeTrait specialization mapping MaterialInstance to AssetTypes::MaterialInstance.
    template <>
    struct AssetTypeTrait<MaterialInstance>
    {
        /// @brief The asset type tag for MaterialInstance.
        static constexpr AssetTypeId Type = AssetTypes::MaterialInstance;
    };
}
