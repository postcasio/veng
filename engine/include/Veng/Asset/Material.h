#pragma once

#include <span>
#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetBuild.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/GraphicsPipeline.h>

namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;
}

namespace Veng
{
    struct Shader;
    class Texture;

    /// @brief Selects a material's output contract, pipeline shape, standard vertex shader, and invocation site.
    ///
    /// Surface writes the g-buffer and is drawn per submesh by the geometry pass; PostProcess writes a
    /// single final color and is invoked fullscreen by the post chain; Sky writes background radiance
    /// and is invoked fullscreen in the sky slot (composited over the lit scene with LoadOp::Load);
    /// Translucent writes final HDR radiance and is drawn per submesh by the forward translucent pass
    /// (alpha-blended into the lit scene color, after deferred lighting and the sky, before the
    /// bloom/tonemap tail). The parameter schema, bindless handles, authoring, and editor inspector are
    /// shared across domains. Surface is 0 so a zero-initialized header defaults to the Surface domain.
    enum class MaterialDomain : u32
    {
        /// @brief G-buffer MRT output; drawn per submesh by the geometry pass.
        Surface = 0,
        /// @brief Single color output; invoked fullscreen by the post chain.
        PostProcess = 1,
        /// @brief Background sky radiance; invoked fullscreen in the sky slot, composited over lit scene color.
        Sky = 2,
        /// @brief Final HDR radiance + alpha; drawn per submesh by the forward translucent pass, alpha-blended into the lit scene color.
        Translucent = 3,
    };

    /// @brief One reflected material parameter field, kept at runtime for name-based SetTexture/SetParam dispatch.
    struct MaterialField
    {
        /// @brief Distinguishes authored scalar/vector params from bindless handle slots.
        enum class FieldKind : u32
        {
            Param = 0,
            TextureHandle = 1,
            SamplerHandle = 2,
            /// @brief A runtime-bound byte-address storage-buffer slot (experimental); no cooked default.
            StorageBufferHandle = 3
        };

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

        /// @brief Authored face-culling mode for the material's geometry pipeline.
        ///
        /// Back (the default) matches the engine's opaque convention; None renders both
        /// faces (a two-sided surface). Applied by whichever side builds the pipeline —
        /// the loader for Surface, the owning pass for a pass-built domain.
        Renderer::CullMode CullMode = Renderer::CullMode::Back;

        /// @brief Authored translucent draw-order priority (default 0).
        ///
        /// Translucent draws sort back-to-front within ascending priority groups, so a
        /// higher-priority material draws after — over — every lower-priority one regardless
        /// of depth (an overlay plane, a reticle). Ignored outside the Translucent domain.
        i32 SortPriority = 0;

        /// @brief Null for PostProcess materials (built by the pass).
        Ref<Renderer::GraphicsPipeline> Pipeline;

        /// @brief Kept resident for the material's lifetime.
        AssetHandle<Shader> VertexShader;
        /// @brief Kept resident for the material's lifetime.
        AssetHandle<Shader> FragmentShader;
        /// @brief Texture dependencies kept resident for the material's lifetime.
        vector<AssetHandle<Texture>> Textures;

        /// @brief The default parameter block: bindless handle slots (patched at Finalize) and authored scalar/vector params.
        vector<std::byte> Block;
        /// @brief Reflected field table describing the parameter block layout.
        vector<MaterialField> Fields;
        /// @brief Push-constant offset of the per-draw material selector, or NoSelectorPush.
        ///
        /// A PostProcess material pushes its selector at offset 0. A Surface material reads its
        /// material index from the per-draw DrawData SSBO (indexed by the candidate id), not a
        /// push, so it carries NoSelectorPush.
        u32 SelectorOffset = 0;
    };

    /// @brief A parent material: shader → pipeline, the exposed-param schema, and the default param block.
    ///
    /// A parent owns the expensive half — the graphics pipeline (built from reflected shader layouts,
    /// set 0 is the BindlessRegistry, sets >= 1 from reflection), the pipeline layout, the resident
    /// shader/texture dependencies, the reflected MaterialField schema, and the cooked default
    /// parameter block (held as bytes, with bindless handle slots patched at Finalize). It owns **no**
    /// per-draw SSBO slot and **no** per-instance mutators — those live on MaterialInstance, the cheap
    /// override that owns a slot seeded from this parent's default block. Many instances share one
    /// parent's pipeline and differ only by a per-material slot.
    class Material
    {
    public:
        /// @brief SelectorOffset sentinel: the material pushes no selector (it reads its index from the DrawData SSBO).
        static constexpr u32 NoSelectorPush = ~0u;

        ~Material();

        Material(const Material&) = delete;
        Material& operator=(const Material&) = delete;

        /// @brief Returns the material's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns the material's domain (Surface, PostProcess, Sky, or Translucent).
        [[nodiscard]] MaterialDomain GetDomain() const { return m_Domain; }

        /// @brief Returns the material's authored face-culling mode (Back when unauthored).
        ///
        /// A pass building its own pipeline for this material (Translucent) applies this
        /// instead of assuming the opaque Back convention.
        [[nodiscard]] Renderer::CullMode GetCullMode() const { return m_CullMode; }

        /// @brief Returns the authored translucent draw-order priority (0 when unauthored).
        ///
        /// The translucent pass sorts its draws back-to-front within ascending priority
        /// groups, so a higher-priority material draws over every lower-priority one.
        [[nodiscard]] i32 GetSortPriority() const { return m_SortPriority; }

        /// @brief Returns the built graphics pipeline, or null for a pass-built domain (PostProcess, Sky, Translucent).
        [[nodiscard]] const Ref<Renderer::GraphicsPipeline>& GetPipeline() const
        {
            return m_Pipeline;
        }

        /// @brief Returns the reflected pipeline layout (set 0 reserved for bindless; selector push range at the domain's offset).
        ///
        /// A PostProcess pass builds its GraphicsPipeline against this layout using the material's shader modules.
        [[nodiscard]] const Ref<Renderer::PipelineLayout>& GetPipelineLayout() const
        {
            return m_PipelineLayout;
        }

        /// @brief Returns the vertex shader module.
        [[nodiscard]] const Ref<Renderer::ShaderModule>& GetVertexModule() const;

        /// @brief Returns the fragment shader module.
        [[nodiscard]] const Ref<Renderer::ShaderModule>& GetFragmentModule() const;

        /// @brief Returns the push-constant offset of the per-draw material selector.
        ///
        /// Surface → NoSelectorPush; PostProcess → 0 (no geometry block). A pass building its own
        /// pipeline reads this to size its push range.
        [[nodiscard]] u32 GetSelectorOffset() const { return m_SelectorOffset; }

        /// @brief Returns the material's resolved texture dependencies.
        ///
        /// Sampled bindlessly (set 0), so they are invisible to the render graph. A caller
        /// drawing with this material must PrepareForAccess(tex->GetView(), Sample) before
        /// the draw so any async-uploaded texture is acquired onto the graphics queue.
        [[nodiscard]] std::span<const AssetHandle<Texture>> GetTextures() const
        {
            return m_Textures;
        }

        /// @brief Returns the reflected field table describing the material's parameter schema.
        ///
        /// A field's Kind indicates whether it is a bindless handle slot or an authored param.
        /// An instance validates its overrides against this; an editor reads it rather than
        /// re-reflecting the shader.
        [[nodiscard]] std::span<const MaterialField> GetFields() const { return m_Fields; }

        /// @brief Returns the cooked default parameter block (handle slots patched at Finalize).
        ///
        /// A MaterialInstance copies this as the seed for its own SSBO slot, then applies its
        /// overrides. Valid only after Finalize().
        [[nodiscard]] std::span<const std::byte> GetDefaultBlock() const { return m_Block; }

    private:
        friend class MaterialLoader;
        friend class MaterialInstance;
        friend Task<Detail::BuiltAsset<Material>>
        Detail::SubmitAssetBuild(Renderer::Context& context, TaskSystem& tasks, MaterialInfo data,
                                 Ref<Renderer::PipelineLayout> layout);
        friend Ref<Material> Detail::BuildAssetSync(Renderer::Context& context,
                                                    const MaterialInfo& data,
                                                    Ref<Renderer::PipelineLayout> layout);

        /// @brief Constructs an unfinalized Material from the given info.
        ///
        /// The worker-legal construction step; the result must be Finalize()d on the render thread
        /// (the pipeline build + the default-block handle patch) before use.
        /// @param info Material description (shaders, textures, parameter block, fields).
        /// @return The unfinalized material.
        static Ref<Material> Prepare(const MaterialInfo& info)
        {
            return Ref<Material>(new Material(info));
        }

        /// @brief Patches bindless indices into the default block and stores the pipeline + layout.
        ///
        /// Runs on the render thread. The layout and pipeline are supplied here because their GPU
        /// build is also deferred render-thread work. A PostProcess, Sky, or Translucent material is
        /// finalized with a null pipeline — its GraphicsPipeline is built later by the pass that
        /// owns it (PostProcessScenePass / SkyMaterialScenePass / TranslucentScenePass), which alone
        /// knows the renderer's color format. No SSBO slot is allocated: a parent owns no per-draw slot.
        /// @param layout   The reflected pipeline layout (set 0 reserved for bindless).
        /// @param pipeline The built graphics pipeline, or null for PostProcess materials.
        void Finalize(Ref<Renderer::PipelineLayout> layout,
                      Ref<Renderer::GraphicsPipeline> pipeline);

        explicit Material(const MaterialInfo& info);

        Renderer::Context& m_Context;
        string m_Name;
        MaterialDomain m_Domain = MaterialDomain::Surface;
        Renderer::CullMode m_CullMode = Renderer::CullMode::Back;
        i32 m_SortPriority = 0;
        Ref<Renderer::GraphicsPipeline> m_Pipeline;
        Ref<Renderer::PipelineLayout> m_PipelineLayout;

        AssetHandle<Shader> m_VertexShader;
        AssetHandle<Shader> m_FragmentShader;
        vector<AssetHandle<Texture>> m_Textures;

        vector<std::byte> m_Block;
        vector<MaterialField> m_Fields;
        u32 m_SelectorOffset = 0;
        bool m_Finalized = false;
    };

    /// @brief AssetTypeTrait specialization mapping Material to AssetTypes::Material.
    template <>
    struct AssetTypeTrait<Material>
    {
        /// @brief The asset type tag for Material.
        static constexpr AssetTypeId Type = AssetTypes::Material;
    };
}

VE_ENUM(::Veng::MaterialDomain, 0x34CF0B4F57300AF1ULL)
VE_ENUMERATOR(Surface)
VE_ENUMERATOR(PostProcess)
VE_ENUMERATOR(Sky)
VE_ENUMERATOR(Translucent)
VE_ENUM_END();

// The cull-mode name table lives beside MaterialDomain's: material sources are the one
// place a CullMode is serialized by enumerator name, and Renderer/Types.h carries no
// reflection dependency.
VE_ENUM(::Veng::Renderer::CullMode, 0xA9FD0C3F45DF165AULL)
VE_ENUMERATOR(None)
VE_ENUMERATOR(Front)
VE_ENUMERATOR(Back)
VE_ENUMERATOR(FrontAndBack)
VE_ENUM_END();
