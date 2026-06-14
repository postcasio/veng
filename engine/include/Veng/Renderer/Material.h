#pragma once

#include <span>
#include <string_view>

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/ShaderAsset.h>
#include <Veng/Renderer/Texture.h>

// Material (planset-5 plan 09): the thin, bindless end-state material. A loaded
// material owns its forward graphics pipeline (built from the vertex+fragment
// shaders' reflected layout — set 0 is the BindlessRegistry, sets >= 1 from
// reflection), a MaterialData entry in the registry's per-material SSBO array,
// and keeps its shader + texture dependencies resident. Bind() binds the
// pipeline and pushes the material's index as the per-draw selector; it does NOT
// swap descriptor sets per draw — set 0 is bound once per frame by the registry.
//
// This is v1-forward and expected to evolve with the renderer architecture (see
// plan 09's notes); the durable contract is the asset/cook side, not the runtime
// binding shape.
namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;

    // One reflected MaterialData field, kept at runtime so name-based
    // SetTexture/SetParam can resolve a field by Name (mirrors the cooked
    // CookedMaterialField table).
    struct MaterialField
    {
        enum class FieldKind : u32 { Param = 0, TextureHandle = 1, SamplerHandle = 2 };

        string Name;
        u32 Offset = 0;
        u32 Size = 0;
        FieldKind Kind{};
    };

    // Everything the MaterialLoader assembles before constructing the Material.
    struct MaterialInfo
    {
        string Name;
        Context* Context = nullptr;

        Ref<GraphicsPipeline> Pipeline;

        // Eager dependencies kept resident for the material's lifetime.
        AssetHandle<ShaderAsset> VertexShader;
        AssetHandle<ShaderAsset> FragmentShader;
        vector<AssetHandle<Texture>> Textures;

        // The packed MaterialData block (handle slots already patched with the
        // resolved bindless handles), its reflected field table, and the
        // push-constant offset of the per-draw material selector.
        MaterialData Params{};
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

        // Binds the material's pipeline and pushes its index as the per-draw
        // selector. Set 0 (the bindless registry) must already be bound for the
        // frame. Issue the mesh draws after this.
        void Bind(CommandBuffer& cmd) const;

        // Name-based edits (resolved through the reflected field table); both
        // rewrite the material's SSBO entry in place.
        void SetTexture(std::string_view name, AssetHandle<Texture> texture);
        void SetParam(std::string_view name, const vec4& value);

        // The material's slot in the registry's per-material SSBO array.
        [[nodiscard]] u32 GetIndex() const { return m_Handle.Index; }

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] const Ref<GraphicsPipeline>& GetPipeline() const { return m_Pipeline; }

    private:
        explicit Material(const MaterialInfo& info);

        [[nodiscard]] const MaterialField* FindField(std::string_view name) const;
        void UploadParams() const;

        Context& m_Context;
        string m_Name;
        Ref<GraphicsPipeline> m_Pipeline;

        AssetHandle<ShaderAsset> m_VertexShader;
        AssetHandle<ShaderAsset> m_FragmentShader;
        vector<AssetHandle<Texture>> m_Textures;

        MaterialData m_Params{};
        vector<MaterialField> m_Fields;
        u32 m_SelectorOffset = 0;
        MaterialHandle m_Handle;
    };
}

namespace Veng
{
    template <>
    struct AssetTypeTrait<Renderer::Material>
    {
        static constexpr AssetType Type = AssetType::Material;
    };
}
