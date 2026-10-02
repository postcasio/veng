#include "SpriteScenePass.h"

#include <algorithm>
#include <cstring>

#include <glm/geometric.hpp>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Flipbook.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/FlipbookSystem.h>
#include <Veng/Scene/Scene.h>

namespace Veng::Renderer
{
    namespace
    {
        // The sprite shaders in the engine core pack (auto-mounted by AssetManager). The masked
        // fragment writes the bloom mask as a second output; the plain one writes colour alone.
        constexpr AssetId SpriteVertId{0xA37C1AA90FA4C7DFULL};
        constexpr AssetId SpriteFragId{0x107B3F89B7A7C029ULL};
        constexpr AssetId SpriteMaskedFragId{0xE770831C56E352E9ULL};

        // Six vertices (two triangles) per sprite quad.
        constexpr u32 SpriteVertexCount = 6;

        // Matches sprite.vert/frag.slang's push block.
        struct SpritePushConstants
        {
            u32 ViewConstantsIndex;
            u32 Pad0;
            u32 Pad1;
            u32 Pad2;
        };

        // Additive over the lit colour, leaving the target's alpha alone.
        BlendState SpriteAdditiveBlend()
        {
            return {
                .Enable = true,
                .SrcColorFactor = BlendFactor::One,
                .DstColorFactor = BlendFactor::One,
                .ColorOp = BlendOp::Add,
                .SrcAlphaFactor = BlendFactor::Zero,
                .DstAlphaFactor = BlendFactor::One,
                .AlphaOp = BlendOp::Add,
            };
        }

        AssetHandle<Veng::Shader> LoadShader(AssetManager& assets, const AssetId id,
                                             const char* what)
        {
            const AssetResult<AssetHandle<Veng::Shader>> shader = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(shader.has_value(), "SpriteScenePass: {} load failed: {}", what,
                      shader.error().Detail);
            return *shader;
        }

        // The resolved compositing: the sprite's own choice, or its flipbook's recommendation.
        bool IsAdditive(const FlipbookSprite& sprite, const Flipbook& flipbook)
        {
            switch (sprite.Blend)
            {
            case SpriteBlend::Alpha:
                return false;
            case SpriteBlend::Additive:
                return true;
            case SpriteBlend::Asset:
                break;
            }
            return flipbook.GetBlend() == FlipbookBlend::Additive;
        }
    }

    void GatherSprites(const Scene& scene, const CameraView& camera, const f32 alpha,
                       SpriteDrawPlan& plan)
    {
        plan.Alpha.clear();
        plan.Additive.clear();
        plan.Dropped = 0;

        // The alpha set sorts on view-space depth; the additive set needs no order.
        vector<std::pair<f32, GpuSprite>> sorted;
        const mat4 view = camera.View();
        u32 gathered = 0;

        for (auto [entity, sprite] : scene.View<FlipbookSprite>())
        {
            const Flipbook* flipbook = sprite.Flipbook.Get();
            if (flipbook == nullptr || flipbook->GetAtlas() == nullptr || sprite.Finished ||
                sprite.Opacity <= 0.0f || !scene.Has<Transform>(entity))
            {
                continue;
            }
            if (gathered >= MaxSpritesPerFrame)
            {
                ++plan.Dropped;
                continue;
            }
            ++gathered;

            const mat4 world = scene.GetInterpolatedWorldTransform(entity, alpha);
            const vec3 position(world[3]);
            const f32 scale = glm::length(vec3(world[0]));

            f32 width = sprite.Size;
            if (width <= 0.0f)
            {
                width = flipbook->GetWorldExtent() ? flipbook->GetWorldExtent()->x : 1.0f;
            }
            width *= scale;

            const Texture& atlas = *flipbook->GetAtlas();
            const u32 frame = SpriteFlipbookFrame(sprite, flipbook->GetClip());
            const GpuSprite record{
                .PositionWidth = vec4(position, width),
                .Color = vec4(sprite.Tint, sprite.Opacity),
                .UvRect = flipbook->GetFrameRect(frame),
                .Anchor = flipbook->GetAnchor(),
                .Height = width * flipbook->GetAspect(),
                .Rotation = sprite.Rotation,
                .Texture = atlas.GetHandle().Index,
                .Sampler = atlas.GetSamplerHandle().Index,
                .AlphaMode = static_cast<u32>(flipbook->GetAlphaMode()),
            };

            if (IsAdditive(sprite, *flipbook))
            {
                plan.Additive.push_back(record);
            }
            else
            {
                const f32 viewZ = (view * vec4(position, 1.0f)).z;
                sorted.emplace_back(viewZ, record);
            }
        }

        // The camera looks down -Z, so the most negative view-space z is the farthest sprite.
        std::ranges::stable_sort(sorted, {}, &std::pair<f32, GpuSprite>::first);
        plan.Alpha.reserve(sorted.size());
        for (const auto& [depth, record] : sorted)
        {
            plan.Alpha.push_back(record);
        }
    }

    SpriteScenePass::SpriteScenePass(Context& context, AssetManager& assets,
                                     const SpriteDrawPlan* plan, const ResourceId targetId,
                                     const ResourceId depthId, const ResourceId maskId,
                                     const Format targetFormat, const Format maskFormat,
                                     const u32 framesInFlight)
        : m_Context(context), m_Plan(plan), m_TargetId(targetId), m_DepthId(depthId),
          m_MaskId(maskId), m_FramesInFlight(framesInFlight)
    {
        const AssetHandle<Veng::Shader> vs = LoadShader(assets, SpriteVertId, "sprite vertex");
        const AssetHandle<Veng::Shader> fs = LoadShader(
            assets, maskId.IsValid() ? SpriteMaskedFragId : SpriteFragId, "sprite fragment");

        // Set 3 carries the per-frame record SSBO, off bindless: a closed per-frame
        // producer→consumer buffer needs no global registration.
        m_SetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "Sprite Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageBuffer,
                                         .Count = 1,
                                         .Stages = ShaderStage::Vertex}},
                       });
        m_Layout = PipelineLayout::Create(
            m_Context, {
                           .Name = "Sprite Layout",
                           .DescriptorSetLayouts = {m_SetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<SpritePushConstants>(
                               ShaderStage::Vertex | ShaderStage::Fragment)},
                       });

        const auto makePipeline = [&](const char* name, const BlendState& colorBlend)
        {
            vector<PipelineAttachmentInfo> attachments = {
                {.Format = targetFormat, .Blend = colorBlend}};
            if (m_MaskId.IsValid())
            {
                // The mask accumulates like the translucent pass's: two glows over one pixel sum.
                attachments.push_back({.Format = maskFormat, .Blend = BlendState::Additive()});
            }
            return GraphicsPipeline::Create(
                m_Context, {
                               .Name = name,
                               .ColorAttachments = std::move(attachments),
                               .DepthAttachmentFormat = GBuffer::DepthFormat,
                               .PipelineLayout = m_Layout,
                               .ShaderStages =
                                   {
                                       {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                       {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                                   },
                               .Topology = PrimitiveTopology::TriangleList,
                               // A rolled quad may present either winding; a sprite has no back.
                               .CullMode = CullMode::None,
                               .DepthTestEnable = true,
                               .DepthWriteEnable = false,
                               // Reverse-Z: a nearer fragment has larger depth.
                               .DepthCompareOp = CompareOp::GreaterOrEqual,
                           });
        };
        m_AlphaPipeline = makePipeline("Sprite Alpha Pipeline", BlendState::PremultipliedAlpha());
        m_AdditivePipeline = makePipeline("Sprite Additive Pipeline", SpriteAdditiveBlend());

        m_RegionStride = static_cast<u64>(MaxSpritesPerFrame) * sizeof(GpuSprite);
        m_Records = Buffer::Create(m_Context, {
                                                  .Name = "Sprite Records",
                                                  .Size = m_RegionStride * m_FramesInFlight,
                                                  .Usage = BufferUsage::Storage,
                                                  .HostMapped = true,
                                              });

        // One set per frame in flight, each bound once to its own ring region: rewriting a shared
        // set still referenced by a pending command buffer is a validation error.
        m_Sets.reserve(m_FramesInFlight);
        for (u32 frame = 0; frame < m_FramesInFlight; ++frame)
        {
            Ref<DescriptorSet> set =
                DescriptorSet::Create(m_Context, {.Name = "Sprite Set", .Layout = m_SetLayout});
            set->Write(0, m_Records, static_cast<u64>(frame) * m_RegionStride, m_RegionStride);
            m_Sets.push_back(std::move(set));
        }
    }

    SpriteScenePass::~SpriteScenePass() = default;

    void SpriteScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        RenderGraph::PassBuilder builder = graph.AddPass("Scene Sprites");
        builder.Color({
            .Resource = m_TargetId,
            .Load = LoadOp::Load,
            .Store = StoreOp::Store,
        });
        if (m_MaskId.IsValid())
        {
            // The translucent pass ahead of this one cleared the mask; sprites add to it.
            builder.Color({
                .Resource = m_MaskId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            });
        }
        builder.Depth({
            .Resource = m_DepthId,
            .Load = LoadOp::Load,
            .Store = StoreOp::Store,
        });
        // An idle wired pass (deactivation hysteresis) skips its frame rather than loading and
        // storing its targets around no draws.
        builder.SkipWhen([this] { return m_Plan == nullptr || m_Plan->IsEmpty(); });
        builder.Execute([this](PassContext& inner) { Record(Wrap(inner)); });
    }

    u32 SpriteScenePass::Upload() const
    {
        const SpriteDrawPlan& plan = *m_Plan;
        const auto count = static_cast<u32>(
            std::min<usize>(plan.Alpha.size() + plan.Additive.size(), MaxSpritesPerFrame));
        const u32 region = m_Context.GetCurrentFrameInFlight();
        auto* base = static_cast<u8*>(m_Records->GetMappedData()) +
                     static_cast<usize>(region) * m_RegionStride;

        // Alpha first, then additive, so each set is one contiguous run of the region.
        const usize alphaCount = std::min<usize>(plan.Alpha.size(), count);
        std::memcpy(base, plan.Alpha.data(), alphaCount * sizeof(GpuSprite));
        std::memcpy(base + alphaCount * sizeof(GpuSprite), plan.Additive.data(),
                    (count - alphaCount) * sizeof(GpuSprite));
        return count;
    }

    void SpriteScenePass::Record(const ScenePassContext& ctx) const
    {
        CommandBuffer& cmd = ctx.Cmd();
        const uvec2 extent = ctx.View().RenderExtent;
        cmd.SetViewport({0, 0}, extent);
        cmd.SetScissor({0, 0}, extent);

        if (m_Plan == nullptr || m_Plan->IsEmpty())
        {
            return;
        }

        const u32 count = Upload();
        const auto alphaCount = static_cast<u32>(std::min<usize>(m_Plan->Alpha.size(), count));
        const u32 additiveCount = count - alphaCount;

        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const SpritePushConstants push{.ViewConstantsIndex =
                                           registry.GetCurrentViewConstantsIndex()};
        const u32 frame = m_Context.GetCurrentFrameInFlight();

        // The record index is the vertex index over six, so a set's first vertex selects its run.
        const auto draw =
            [&](const Ref<GraphicsPipeline>& pipeline, const u32 first, const u32 sprites)
        {
            cmd.BindPipeline(pipeline);
            registry.Bind(cmd);
            cmd.BindDescriptorSets(DescriptorSetBindInfo{
                .Sets = {m_Sets[frame]},
                .FirstSet = BindlessRegistry::FirstUserSet,
                .PipelineBindPoint = PipelineBindPoint::Graphics,
            });
            cmd.PushConstants(push);
            cmd.Draw(sprites * SpriteVertexCount, 1, first * SpriteVertexCount, 0);
        };
        if (alphaCount > 0)
        {
            draw(m_AlphaPipeline, 0, alphaCount);
        }
        if (additiveCount > 0)
        {
            draw(m_AdditivePipeline, alphaCount, additiveCount);
        }
    }
}
