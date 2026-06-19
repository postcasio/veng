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

// Material: the thin, bindless end-state material. A loaded material owns its
// forward graphics pipeline (built from the vertex+fragment shaders' reflected
// layout — set 0 is the BindlessRegistry, sets >= 1 from reflection), one
// parameter-block entry in the registry's per-material buffer, and keeps its
// shader + texture dependencies resident. Bind() binds the pipeline and pushes
// the material's index as the per-draw selector; it does NOT swap descriptor
// sets per draw — set 0 is bound once per frame by the registry.
namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;
}

namespace Veng
{
    // A material's domain selects its output contract, pipeline shape, standard
    // vertex shader, and invocation site. Surface writes the g-buffer and is drawn
    // per submesh by the geometry pass; PostProcess writes a single final color and
    // is invoked fullscreen by the post chain. The rest of the material system —
    // parameter schema, bindless handles, authoring, inspector — is shared across
    // domains. Surface is 0 so a zero-initialised header reads as the existing
    // behavior.
    enum class MaterialDomain : u32
    {
        Surface = 0,
        PostProcess = 1,
    };

    // One reflected material field, kept at runtime so name-based
    // SetTexture/SetParam can resolve a field by Name (mirrors the cooked
    // CookedMaterialField table).
    struct MaterialField
    {
        enum class FieldKind : u32 { Param = 0, TextureHandle = 1, SamplerHandle = 2 };

        string Name;
        u32 Offset = 0;
        u32 Size = 0;
        FieldKind Kind{};
        // For handle fields, the AssetId of the texture whose bindless index is
        // written here at Finalize() (0 for plain Param fields).
        u64 TextureId = 0;
    };

    // Everything the MaterialLoader assembles before constructing the Material.
    struct MaterialInfo
    {
        string Name;
        Renderer::Context* Context = nullptr;

        MaterialDomain Domain = MaterialDomain::Surface;

        Ref<Renderer::GraphicsPipeline> Pipeline;

        // Eager dependencies kept resident for the material's lifetime.
        AssetHandle<Shader> VertexShader;
        AssetHandle<Shader> FragmentShader;
        vector<AssetHandle<Texture>> Textures;

        // The single parameter block — bindless handle slots (patched at Finalize
        // with the resolved indices) and authored scalar/vector params (packed at
        // cook), laid out by reflection at each field's offset — the reflected
        // field table, and the push-constant offset of the per-draw material
        // selector.
        vector<std::byte> Block;
        vector<MaterialField> Fields;
        u32 SelectorOffset = 0;
    };

    class Material
    {
    public:
        static Ref<Material> Create(const MaterialInfo& info)
        {
            return Ref<Material>(new Material(info));
        }

        ~Material();

        Material(const Material&) = delete;
        Material& operator=(const Material&) = delete;

        // Patch the resolved texture/sampler bindless indices into the param
        // block (the textures must already be Finalize()d), allocate the
        // per-material SSBO slot, and upload. Runs on the main thread; the
        // layout + pipeline are supplied here because their GPU build is also
        // main-thread work the async loader defers to the continuation. A
        // PostProcess material is finalized with a null pipeline — its
        // GraphicsPipeline is built later by the PostProcessScenePass against the
        // renderer's color format, which the loader cannot know — so it keeps the
        // layout + shader modules and binds nothing on Material::Bind beyond
        // pushing the selector.
        void Finalize(Ref<Renderer::PipelineLayout> layout, Ref<Renderer::GraphicsPipeline> pipeline);

        // Binds the material's pipeline and pushes its index as the per-draw
        // selector. Set 0 (the bindless registry) must already be bound for the
        // frame. Issue the mesh draws after this. A PostProcess material has no
        // owned pipeline (the pass binds its own); Bind only pushes the selector,
        // at the domain's selector offset.
        void Bind(Renderer::CommandBuffer& cmd) const;

        // Name-based edits (resolved through the reflected field table); both
        // rewrite the material's SSBO entry in place.
        void SetTexture(std::string_view name, AssetHandle<Texture> texture);
        void SetParam(std::string_view name, const vec4& value);

        // Write a raw bindless index into a handle field by name — the path a
        // runtime-bound input takes (a renderer-owned ImageView/Sampler the
        // material does not own as a cooked Texture asset). SetTextureHandle
        // targets a TextureHandle field, SetSamplerHandle a SamplerHandle field;
        // neither keeps an asset resident. The write lands in the ring-buffered
        // block's current frame region, so it is cheap and frame-safe per frame.
        void SetTextureHandle(std::string_view name, Renderer::TextureHandle handle);
        void SetSamplerHandle(std::string_view name, Renderer::SamplerHandle handle);

        // The material's slot in the registry's per-material SSBO array.
        [[nodiscard]] u32 GetIndex() const { return m_Handle.Index; }

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] MaterialDomain GetDomain() const { return m_Domain; }
        [[nodiscard]] const Ref<Renderer::GraphicsPipeline>& GetPipeline() const { return m_Pipeline; }

        // The reflected pipeline layout (built by the loader for both domains: set
        // 0 reserved for the bindless registry, the selector push range at the
        // domain's offset). A PostProcess pass builds its GraphicsPipeline against
        // this layout and the material's shader modules. The shader modules supply
        // that pass's fullscreen vertex + fragment stages.
        [[nodiscard]] const Ref<Renderer::PipelineLayout>& GetPipelineLayout() const { return m_PipelineLayout; }
        [[nodiscard]] const Ref<Renderer::ShaderModule>& GetVertexModule() const { return m_VertexShader.Get()->Module; }
        [[nodiscard]] const Ref<Renderer::ShaderModule>& GetFragmentModule() const { return m_FragmentShader.Get()->Module; }

        // The push-constant offset of the per-draw material selector (Surface →
        // 64, after the MVP; PostProcess → 0, no geometry block). A pass building
        // its own pipeline reads this to size its push range.
        [[nodiscard]] u32 GetSelectorOffset() const { return m_SelectorOffset; }

        // The material's resolved texture dependencies. Sampled bindlessly (set
        // 0), so they are invisible to the render graph: a caller drawing with
        // this material must PrepareForAccess(tex->GetView(), Sample) before the
        // draw so an async-uploaded texture is acquired onto the graphics queue
        // and its transfer-timeline wait folded into the frame submit.
        [[nodiscard]] std::span<const AssetHandle<Texture>> GetTextures() const { return m_Textures; }

        // The material's reflected field table — its parameter schema. A field's
        // Kind tells a consumer whether it is a texture/sampler handle slot or an
        // authored scalar/vector param; both live in the one parameter block.
        // An editor reads this rather than re-reflecting the shader.
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

    template <>
    struct AssetTypeTrait<Material>
    {
        static constexpr AssetType Type = AssetType::Material;
    };
}
