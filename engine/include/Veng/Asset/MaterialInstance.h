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
    /// frame-in-flight region.
    ///
    /// A bare parent Material doubles as a zero-override instance — the MaterialInstanceLoader
    /// resolves a Material-typed id to a default instance, so existing material-id references keep
    /// loading.
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

        /// @brief Returns the frame-folded material selector (GetCurrentFrameBase() + slot index).
        ///
        /// The value the shader uses to index the ring-buffered per-material parameter block. A
        /// Surface draw's geometry pass writes this into each per-draw DrawData record instead of
        /// pushing it; it changes per frame-in-flight, so read it at record time.
        [[nodiscard]] u32 GetMaterialSelector() const;

        /// @brief Sets the texture for a named handle field and rewrites the SSBO entry in place.
        void SetTexture(std::string_view name, AssetHandle<Texture> texture);

        /// @brief Sets a vec4 parameter by field name, rewriting the SSBO entry in place.
        void SetParam(std::string_view name, const vec4& value);

        /// @brief Sets a scalar float parameter by field name.
        ///
        /// Writes only the field's reflected Size bytes, never smearing a vec4 over adjacent fields.
        void SetParam(std::string_view name, f32 value);

        /// @brief Writes a raw bindless texture index into a TextureHandle field by name.
        ///
        /// For runtime-bound inputs (a renderer-owned ImageView the instance does not own as a
        /// cooked Texture asset). Does not keep any asset resident. The write lands in the
        /// ring-buffered block's current frame region — cheap and frame-safe.
        void SetTextureHandle(std::string_view name, Renderer::TextureHandle handle);

        /// @brief Writes a raw bindless sampler index into a SamplerHandle field by name.
        ///
        /// Same semantics as SetTextureHandle but targets a SamplerHandle field.
        void SetSamplerHandle(std::string_view name, Renderer::SamplerHandle handle);

        /// @brief The additive emissive term an instance contributes to the forward emissive pass.
        ///
        /// Resolved from the instance's own (override-patched) parameter block: the rgb color
        /// from an EmissiveColor vec4 field and, where authored, an EmissiveTexture handle field
        /// modulating it. A material lacking an EmissiveColor field emits nothing (zero color).
        struct EmissiveParams
        {
            /// @brief rgb additive emissive color; zero when the material declares no EmissiveColor.
            vec3 Color;
            /// @brief Bindless texture handle index modulating Color, or 0 for a flat color.
            u32 Texture = 0;
            /// @brief Bindless sampler handle index for Texture; unused when Texture is 0.
            u32 Sampler = 0;
        };

        /// @brief Reads this instance's emissive term from its parameter block.
        ///
        /// @return The resolved EmissiveParams; zero color when the material has no EmissiveColor field.
        [[nodiscard]] EmissiveParams GetEmissive() const;

        /// @brief Returns the instance's slot index in the registry's per-material SSBO array.
        [[nodiscard]] u32 GetIndex() const { return m_Handle.Index; }

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

        /// @brief Constructs an unfinalized instance: copies the parent's default block.
        ///
        /// The worker-legal construction step; the parent must already be resident. The result is
        /// Finalize()d on the render thread (override patch + bindless slot allocation) before use.
        static Ref<MaterialInstance> Prepare(const MaterialInstanceInfo& info)
        {
            return Ref<MaterialInstance>(new MaterialInstance(info));
        }

        /// @brief Applies the overrides, allocates the per-material SSBO slot, and uploads.
        ///
        /// Runs on the render thread (the override textures are resident by now, so their bindless
        /// indices resolve). A param override is copied at its parent field's offset; a texture
        /// override patches the field's handle slot.
        void Finalize();

        explicit MaterialInstance(const MaterialInstanceInfo& info);

        [[nodiscard]] const MaterialField* FindField(std::string_view name) const;
        void UploadParams() const;

        Renderer::Context& m_Context;
        string m_Name;
        AssetHandle<Material> m_Parent;
        vector<MaterialOverride> m_Overrides;
        vector<AssetHandle<Texture>> m_Textures;

        vector<std::byte> m_Block;
        Renderer::MaterialHandle m_Handle;
        bool m_Registered = false;
    };

    /// @brief AssetTypeTrait specialization mapping MaterialInstance to AssetType::MaterialInstance.
    template <>
    struct AssetTypeTrait<MaterialInstance>
    {
        /// @brief The asset type tag for MaterialInstance.
        static constexpr AssetType Type = AssetType::MaterialInstance;
    };
}
