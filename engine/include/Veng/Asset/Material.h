#pragma once

#include <span>
#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/GraphicsPipeline.h>

namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;
}

namespace Veng
{
    /// @brief Selects a material's output contract, pipeline shape, standard vertex shader, and invocation site.
    ///
    /// Surface writes the g-buffer and is drawn per submesh by the geometry pass; PostProcess writes a
    /// single final color and is invoked fullscreen by the post chain. The parameter schema, bindless
    /// handles, authoring, and editor inspector are shared across domains. Surface is 0 so a
    /// zero-initialized header defaults to the Surface domain.
    enum class MaterialDomain : u32
    {
        /// @brief G-buffer MRT output; drawn per submesh by the geometry pass.
        Surface = 0,
        /// @brief Single color output; invoked fullscreen by the post chain.
        PostProcess = 1,
    };

    /// @brief One reflected material parameter field, kept at runtime for name-based SetTexture/SetParam dispatch.
    struct MaterialField
    {
        /// @brief Distinguishes authored scalar/vector params from bindless handle slots.
        enum class FieldKind : u32 { Param = 0, TextureHandle = 1, SamplerHandle = 2 };

        /// @brief Field name; matched by SetTexture/SetParam.
        string Name;
        /// @brief Byte offset of the field within the parameter block.
        u32 Offset = 0;
        /// @brief Size in bytes.
        u32 Size = 0;
        /// @brief Whether this field is a param or a bindless handle slot.
        FieldKind Kind{};
        /// @brief For handle fields, the AssetId of the texture whose bindless index is written here at Finalize(); 0 for Param fields.
        u64 TextureId = 0;
    };

    /// @brief Construction parameters for Material, assembled by MaterialLoader before calling Create.
    struct MaterialInfo
    {
        /// @brief Debug name for the material.
        string Name;
        /// @brief Render context used for resource creation.
        Renderer::Context* Context = nullptr;

        /// @brief Output contract and pipeline shape.
        MaterialDomain Domain = MaterialDomain::Surface;

        /// @brief Null for PostProcess materials (built by the pass).
        Ref<Renderer::GraphicsPipeline> Pipeline;

        /// @brief Kept resident for the material's lifetime.
        AssetHandle<Shader> VertexShader;
        /// @brief Kept resident for the material's lifetime.
        AssetHandle<Shader> FragmentShader;
        /// @brief Texture dependencies kept resident for the material's lifetime.
        vector<AssetHandle<Texture>> Textures;

        /// @brief The parameter block: bindless handle slots (patched at Finalize) and authored scalar/vector params.
        vector<std::byte> Block;
        /// @brief Reflected field table describing the parameter block layout.
        vector<MaterialField> Fields;
        /// @brief Push-constant offset of the per-draw material selector.
        u32 SelectorOffset = 0;
    };

    /// @brief Thin, bindless end-state material.
    ///
    /// A loaded material owns its graphics pipeline (built from reflected shader layouts — set 0
    /// is the BindlessRegistry, sets >= 1 from reflection), one parameter-block entry in the
    /// registry's per-material buffer, and keeps its shader and texture dependencies resident.
    /// Bind() binds the pipeline and pushes the material's index as the per-draw selector;
    /// set 0 is bound once per frame by the registry, not per draw.
    class Material
    {
    public:
        /// @brief Creates a Material from the given info.
        static Ref<Material> Create(const MaterialInfo& info)
        {
            return Ref<Material>(new Material(info));
        }

        ~Material();

        Material(const Material&) = delete;
        Material& operator=(const Material&) = delete;

        /// @brief Patches bindless indices, allocates the per-material SSBO slot, and uploads.
        ///
        /// Runs on the main thread. The layout and pipeline are supplied here because their GPU
        /// build is also deferred main-thread work. A PostProcess material is finalized with a
        /// null pipeline — its GraphicsPipeline is built later by the PostProcessScenePass
        /// against the renderer's color format; Bind() then only pushes the selector.
        /// @param layout   The reflected pipeline layout (set 0 reserved for bindless).
        /// @param pipeline The built graphics pipeline, or null for PostProcess materials.
        void Finalize(Ref<Renderer::PipelineLayout> layout, Ref<Renderer::GraphicsPipeline> pipeline);

        /// @brief Binds the material's pipeline and pushes its index as the per-draw selector.
        ///
        /// Set 0 (the bindless registry) must already be bound for the frame. Issue mesh draws
        /// after this call. A PostProcess material has no owned pipeline; only the selector is pushed.
        void Bind(Renderer::CommandBuffer& cmd) const;

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
        /// For runtime-bound inputs (a renderer-owned ImageView the material does not own as a
        /// cooked Texture asset). Does not keep any asset resident. The write lands in the
        /// ring-buffered block's current frame region — cheap and frame-safe.
        void SetTextureHandle(std::string_view name, Renderer::TextureHandle handle);

        /// @brief Writes a raw bindless sampler index into a SamplerHandle field by name.
        ///
        /// Same semantics as SetTextureHandle but targets a SamplerHandle field.
        void SetSamplerHandle(std::string_view name, Renderer::SamplerHandle handle);

        /// @brief Returns the material's slot index in the registry's per-material SSBO array.
        [[nodiscard]] u32 GetIndex() const { return m_Handle.Index; }

        /// @brief Returns the material's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns the material's domain (Surface or PostProcess).
        [[nodiscard]] MaterialDomain GetDomain() const { return m_Domain; }

        /// @brief Returns the built graphics pipeline, or null for a PostProcess material.
        [[nodiscard]] const Ref<Renderer::GraphicsPipeline>& GetPipeline() const { return m_Pipeline; }

        /// @brief Returns the reflected pipeline layout (set 0 reserved for bindless; selector push range at the domain's offset).
        ///
        /// A PostProcess pass builds its GraphicsPipeline against this layout using the material's shader modules.
        [[nodiscard]] const Ref<Renderer::PipelineLayout>& GetPipelineLayout() const { return m_PipelineLayout; }

        /// @brief Returns the vertex shader module.
        [[nodiscard]] const Ref<Renderer::ShaderModule>& GetVertexModule() const { return m_VertexShader.Get()->Module; }

        /// @brief Returns the fragment shader module.
        [[nodiscard]] const Ref<Renderer::ShaderModule>& GetFragmentModule() const { return m_FragmentShader.Get()->Module; }

        /// @brief Returns the push-constant offset of the per-draw material selector.
        ///
        /// Surface → 64 (after the MVP block); PostProcess → 0 (no geometry block).
        /// A pass building its own pipeline reads this to size its push range.
        [[nodiscard]] u32 GetSelectorOffset() const { return m_SelectorOffset; }

        /// @brief Returns the material's resolved texture dependencies.
        ///
        /// Sampled bindlessly (set 0), so they are invisible to the render graph. A caller
        /// drawing with this material must PrepareForAccess(tex->GetView(), Sample) before
        /// the draw so any async-uploaded texture is acquired onto the graphics queue.
        [[nodiscard]] std::span<const AssetHandle<Texture>> GetTextures() const { return m_Textures; }

        /// @brief Returns the reflected field table describing the material's parameter schema.
        ///
        /// A field's Kind indicates whether it is a bindless handle slot or an authored param.
        /// An editor reads this rather than re-reflecting the shader.
        [[nodiscard]] std::span<const MaterialField> GetFields() const { return m_Fields; }

    private:
        explicit Material(const MaterialInfo& info);

        [[nodiscard]] const MaterialField* FindField(std::string_view name) const;
        void UploadParams() const;

        Renderer::Context& m_Context;
        string m_Name;
        MaterialDomain m_Domain = MaterialDomain::Surface;
        Ref<Renderer::GraphicsPipeline> m_Pipeline;
        Ref<Renderer::PipelineLayout> m_PipelineLayout;

        AssetHandle<Shader> m_VertexShader;
        AssetHandle<Shader> m_FragmentShader;
        vector<AssetHandle<Texture>> m_Textures;

        vector<std::byte> m_Block;
        vector<MaterialField> m_Fields;
        u32 m_SelectorOffset = 0;
        Renderer::MaterialHandle m_Handle;
        bool m_Registered = false;
    };

    /// @brief AssetTypeTrait specialization mapping Material to AssetType::Material.
    template <>
    struct AssetTypeTrait<Material>
    {
        /// @brief The asset type tag for Material.
        static constexpr AssetType Type = AssetType::Material;
    };
}
