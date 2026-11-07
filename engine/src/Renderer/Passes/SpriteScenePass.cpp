#include "SpriteScenePass.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

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
        constexpr AssetId SpriteReactiveFragId{0x7D6B0FE48A6066D0ULL};
        constexpr AssetId SpriteMaskedReactiveFragId{0x8F1BCC7D0600B38BULL};

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

    namespace
    {
        // The ramp entry a gathered sprite reads: its override, or its flipbook's default, appended
        // to the plan's table once per frame.
        class RampTable
        {
        public:
            explicit RampTable(SpriteDrawPlan& plan) : m_Plan(plan) {}

            u32 Resolve(const FlipbookSprite& sprite, const Flipbook& flipbook)
            {
                const FlipbookEmission& emission = flipbook.GetEmission();
                const bool overridesRamp = !sprite.EmissionRamp.empty();
                if (!overridesRamp && sprite.Ignition < 0.0f)
                {
                    const auto [it, inserted] =
                        m_Defaults.try_emplace(&flipbook, static_cast<u32>(m_Plan.Ramps.size()));
                    if (inserted)
                    {
                        Append(emission.Ramp, emission.Ignition);
                    }
                    return it->second;
                }

                const vector<FlipbookEmissionKey>& keys =
                    overridesRamp ? sprite.EmissionRamp : emission.Ramp;
                if (keys.size() > MaxFlipbookEmissionKeys)
                {
                    ++m_Plan.RampsClamped;
                }
                const auto index = static_cast<u32>(m_Plan.Ramps.size());
                Append(keys, sprite.Ignition >= 0.0f ? sprite.Ignition : emission.Ignition);
                return index;
            }

        private:
            void Append(const vector<FlipbookEmissionKey>& keys, const f32 ignition)
            {
                const auto count =
                    static_cast<u32>(std::min<usize>(keys.size(), MaxFlipbookEmissionKeys));
                GpuEmissionRamp& ramp = m_Plan.Ramps.emplace_back();
                for (u32 key = 0; key < count; ++key)
                {
                    ramp.Keys[key] = vec4(keys[key].Color, keys[key].T);
                }
                ramp.Params = vec4(ignition, static_cast<f32>(count), 0.0f, 0.0f);
            }

            SpriteDrawPlan& m_Plan;
            std::unordered_map<const Flipbook*, u32> m_Defaults;
        };

        u32 SpriteFlags(const Flipbook& flipbook, const FlipbookFrameBlend blend, const bool emits)
        {
            u32 flags = static_cast<u32>(blend) << SpriteFlag::BlendShift;
            if (flipbook.GetShading() == FlipbookShading::SixWay)
            {
                flags |= SpriteFlag::SixWay;
                if (flipbook.GetTemperatureEncoding() == FlipbookTemperatureEncoding::Sqrt)
                {
                    flags |= SpriteFlag::SqrtTemperature;
                }
                if (flipbook.GetTemperatureChannel() == 0)
                {
                    flags |= SpriteFlag::TemperatureRed;
                }
                if (emits)
                {
                    flags |= SpriteFlag::Emits;
                }
            }
            return flags;
        }
    }

    void GatherSprites(const Scene& scene, const CameraView& camera, const f32 alpha,
                       SpriteDrawPlan& plan)
    {
        plan.Alpha.clear();
        plan.Additive.clear();
        plan.Ramps.clear();
        plan.Dropped = 0;
        plan.RampsClamped = 0;
        plan.ReadsSceneDepth = false;

        // The alpha set sorts on view-space depth; the additive set needs no order.
        vector<std::pair<f32, GpuSprite>> sorted;
        const mat4 view = camera.View();
        u32 gathered = 0;
        RampTable ramps(plan);

        for (auto [entity, sprite] : scene.View<FlipbookSprite>())
        {
            const Flipbook* flipbook = sprite.Flipbook.Get();
            if (flipbook == nullptr || flipbook->GetAtlas() == nullptr || sprite.Finished ||
                sprite.Opacity <= 0.0f || !scene.Has<Transform>(entity))
            {
                continue;
            }
            const bool sixWay = flipbook->GetShading() == FlipbookShading::SixWay;
            if (sixWay &&
                (flipbook->GetLightB() == nullptr || flipbook->GetTemperatureTexture() == nullptr))
            {
                continue;
            }
            if (gathered >= MaxSpritesPerFrame)
            {
                ++plan.Dropped;
                continue;
            }

            const mat4 world = scene.GetInterpolatedWorldTransform(entity, alpha);
            const vec3 position(world[3]);
            const f32 scale = glm::length(vec3(world[0]));

            f32 width = sprite.Size;
            if (width <= 0.0f)
            {
                width = flipbook->GetWorldExtent() ? flipbook->GetWorldExtent()->x : 1.0f;
            }
            width *= scale;

            const SpriteShading shading =
                ResolveSpriteShading(sprite, flipbook->GetShading(), flipbook->GetFrameBlend(),
                                     flipbook->GetMotion() != nullptr, width, scale);
            ++gathered;

            const Texture& atlas = *flipbook->GetAtlas();
            const FlipbookClip& clip = flipbook->GetClip();
            const FlipbookFramePosition frame = SpriteFlipbookFramePosition(sprite, clip);
            const bool blends = shading.FrameBlend != FlipbookFrameBlend::Step;
            const u32 next = blends ? frame.Next : frame.Frame;
            const bool emits =
                sixWay && (!sprite.EmissionRamp.empty() || !flipbook->GetEmission().Ramp.empty());
            const u32 ramp = emits ? ramps.Resolve(sprite, *flipbook) : 0u;

            GpuSprite record{
                .PositionWidth = vec4(position, width),
                .Color = vec4(sprite.Tint, sprite.Opacity),
                .UvRect = flipbook->GetFrameRect(frame.Frame),
                .UvRectNext = flipbook->GetFrameRect(next),
                .Anchor = flipbook->GetAnchor(),
                .Height = width * flipbook->GetAspect(),
                .Rotation = sprite.Rotation,
                .TileSize = vec2(1.0f / static_cast<f32>(flipbook->GetColumns()),
                                 1.0f / static_cast<f32>(flipbook->GetRows())),
                .FrameScale = flipbook->GetFrameScale(frame.Frame),
                .FrameScaleNext = flipbook->GetFrameScale(next),
                .Texture = atlas.GetHandle().Index,
                .Sampler = atlas.GetSamplerHandle().Index,
                .AlphaMode = static_cast<u32>(flipbook->GetAlphaMode()),
                .Flags = SpriteFlags(*flipbook, shading.FrameBlend, emits),
                .Ramp = ramp,
                .FrameFraction = blends ? frame.Fraction : 0.0f,
                .SoftDistance = shading.SoftDistance,
                .EmissionIntensity = sprite.EmissionIntensity,
                .TemperatureOffset = sprite.TemperatureOffset,
            };
            if (sixWay)
            {
                record.LightB = flipbook->GetLightB()->GetHandle().Index;
                record.Temperature = flipbook->GetTemperatureTexture()->GetHandle().Index;
            }
            if (shading.FrameBlend == FlipbookFrameBlend::Motion)
            {
                record.Motion = flipbook->GetMotion()->GetHandle().Index;
                record.MotionScale = flipbook->GetMotionScale();
            }
            plan.ReadsSceneDepth = plan.ReadsSceneDepth || shading.SoftDistance > 0.0f;

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
                                     const SpriteScenePassInfo& info)
        : m_Context(context), m_Plan(info.Plan), m_TargetId(info.Target), m_DepthId(info.Depth),
          m_MaskId(info.Mask), m_ReactiveId(info.Reactive), m_SceneDepthCopyId(info.SceneDepthCopy),
          m_Forward(info.Forward), m_FramesInFlight(info.FramesInFlight)
    {
        const AssetHandle<Veng::Shader> vs = LoadShader(assets, SpriteVertId, "sprite vertex");
        // The entry writes colour, then the bloom mask, then the reactive mask, for those wired.
        const AssetId fragId =
            m_MaskId.IsValid()
                ? (m_ReactiveId.IsValid() ? SpriteMaskedReactiveFragId : SpriteMaskedFragId)
                : (m_ReactiveId.IsValid() ? SpriteReactiveFragId : SpriteFragId);
        const AssetHandle<Veng::Shader> fs = LoadShader(assets, fragId, "sprite fragment");

        // Set 3 carries the per-frame records and ramps, off bindless: a closed per-frame
        // producer→consumer buffer needs no global registration. Sets 4 and 5 are the renderer's
        // IBL and shadow sets, read by the six-way light gather.
        m_SetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "Sprite Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageBuffer,
                                         .Count = 1,
                                         .Stages = ShaderStage::Vertex | ShaderStage::Fragment},
                                        {.Binding = 1,
                                         .Type = DescriptorType::StorageBuffer,
                                         .Count = 1,
                                         .Stages = ShaderStage::Fragment}},
                       });
        m_Layout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "Sprite Layout",
                .DescriptorSetLayouts = {m_SetLayout, m_Forward.IblLayout, m_Forward.ShadowLayout},
                .PushConstantRanges = {PushConstantRange::Of<SpritePushConstants>(
                    ShaderStage::Vertex | ShaderStage::Fragment)},
            });

        const Format targetFormat = info.TargetFormat;
        const Format maskFormat = info.MaskFormat;
        const auto makePipeline = [&](const char* name, const BlendState& colorBlend)
        {
            vector<PipelineAttachmentInfo> attachments = {
                {.Format = targetFormat, .Blend = colorBlend}};
            if (m_MaskId.IsValid())
            {
                // The mask accumulates like the translucent pass's: two glows over one pixel sum.
                attachments.push_back({.Format = maskFormat, .Blend = BlendState::Additive()});
            }
            if (m_ReactiveId.IsValid())
            {
                attachments.push_back(
                    {.Format = info.ReactiveFormat, .Blend = BlendState::Additive()});
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
        // At most one ramp per gathered sprite, so the table cannot overflow while the records fit.
        m_RampRegionStride = static_cast<u64>(MaxSpritesPerFrame) * sizeof(GpuEmissionRamp);
        m_Ramps = Buffer::Create(m_Context, {
                                                .Name = "Sprite Emission Ramps",
                                                .Size = m_RampRegionStride * m_FramesInFlight,
                                                .Usage = BufferUsage::Storage,
                                                .HostMapped = true,
                                            });

        // One set per frame in flight, each bound once to its own ring regions: rewriting a shared
        // set still referenced by a pending command buffer is a validation error.
        m_Sets.reserve(m_FramesInFlight);
        for (u32 frame = 0; frame < m_FramesInFlight; ++frame)
        {
            Ref<DescriptorSet> set =
                DescriptorSet::Create(m_Context, {.Name = "Sprite Set", .Layout = m_SetLayout});
            set->Write(0, m_Records, static_cast<u64>(frame) * m_RegionStride, m_RegionStride);
            set->Write(1, m_Ramps, static_cast<u64>(frame) * m_RampRegionStride,
                       m_RampRegionStride);
            m_Sets.push_back(std::move(set));
        }
    }

    SpriteScenePass::~SpriteScenePass() = default;

    void SpriteScenePass::Declare(RenderGraph& graph, const PassIO& io)
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
        if (m_ReactiveId.IsValid())
        {
            // Cleared ahead of the scene's composited layers; sprites add their coverage.
            builder.Color({
                .Resource = m_ReactiveId,
                .Load = LoadOp::Load,
                .Store = StoreOp::Store,
            });
        }
        builder.Depth({
            .Resource = m_DepthId,
            .Load = LoadOp::Load,
            .Store = StoreOp::Store,
        });
        DeclareInputs(builder, io);
        // An idle wired pass (deactivation hysteresis) skips its frame rather than loading and
        // storing its targets around no draws.
        builder.SkipWhen([this] { return !IsDrawing(); });
        builder.Execute([this](PassContext& inner) { Record(Wrap(inner)); });
    }

    void SpriteScenePass::DeclareInputs(RenderGraph::PassBuilder& builder, const PassIO& io) const
    {
        // A soft sprite's fragment reads the opaque-depth copy, and a six-way sprite's vertex
        // stage the shadow atlases.
        if (m_SceneDepthCopyId.IsValid())
        {
            builder.Sample(m_SceneDepthCopyId);
        }
        if (io.ShadowMap.IsValid())
        {
            builder.Sample(io.ShadowMap, AccessKind::SampleVertexFragment);
        }
        if (io.PunctualShadowMap.IsValid())
        {
            builder.Sample(io.PunctualShadowMap, AccessKind::SampleVertexFragment);
        }
    }

    bool SpriteScenePass::IsDrawing() const
    {
        return m_Plan != nullptr && !m_Plan->IsEmpty();
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

        const usize ramps = std::min<usize>(plan.Ramps.size(), MaxSpritesPerFrame);
        auto* rampBase = static_cast<u8*>(m_Ramps->GetMappedData()) +
                         static_cast<usize>(region) * m_RampRegionStride;
        std::memcpy(rampBase, plan.Ramps.data(), ramps * sizeof(GpuEmissionRamp));
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
            // The shadow rings are framesInFlight-deep, so their dynamic offsets are the
            // frame-in-flight index, as the lighting and forward-lit translucent passes bind them.
            cmd.BindDescriptorSets(
                {m_Sets[frame].get(), m_Forward.IblSet.get(), m_Forward.ShadowSet.get()},
                BindlessRegistry::FirstUserSet, PipelineBindPoint::Graphics,
                {frame * m_Forward.ShadowRingStride, frame * m_Forward.PunctualRingStride});
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
