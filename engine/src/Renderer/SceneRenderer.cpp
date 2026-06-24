#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/ScenePass.h>

#include "ShadowScenePass.h"
#include "PunctualShadowScenePass.h"
#include "SsaoScenePass.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <span>
#include <tuple>
#include <utility>

#include <fmt/format.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/PunctualShadows.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/ShadowCascades.h>
#include <Veng/Renderer/TaaJitter.h>

#include <Veng/Math/AABB.h>
#include <Veng/Math/Frustum.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Shader.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

namespace Veng::Renderer
{
    namespace
    {
        // The engine core pack's fullscreen shaders (the AssetManager auto-mounts the core pack).
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        constexpr AssetId DeferredLightingFragId{0x6569EBAC0810CC1FULL};
        constexpr AssetId DeferredLightingSsaoFragId{0x6EEF5D26BAF2849FULL};
        constexpr AssetId DeferredLightingCascadesFragId{0x834ED7C05F336E01ULL};
        constexpr AssetId SsaoFragId{0xCCBA63DB760A4E8EULL};
        constexpr AssetId TonemapMaterialId{0xBC968C8771B00434ULL};
        constexpr AssetId AlbedoBlitFragId{0xF90F709155D04BE7ULL};
        constexpr AssetId NormalBlitFragId{0x5A2CD7B270EAE5CDULL};
        constexpr AssetId DepthBlitFragId{0xE05F5F86E72F96D5ULL};
        constexpr AssetId OrmBlitFragId{0x7992B54A844CB1E1ULL};
        constexpr AssetId AoBlitFragId{0x97974B40192934E4ULL};
        constexpr AssetId MotionBlitFragId{0xCCD40C76935382FDULL};
        constexpr AssetId ShadowBlitFragId{0x0B61D5D42DAEF190ULL};

        // The TAA resolve and history-copy fragment shaders.
        constexpr AssetId TaaResolveFragId{0xF277BB65AEDAC33EULL};
        constexpr AssetId TaaHistoryCopyFragId{0x07F31C1EC98A29BFULL};

        // The hi-Z max-Z reduction compute shader.
        constexpr AssetId HiZReduceCompId{0xCB20C4EF8A20ADBCULL};

        // The GPU occlusion-cull → indirect-draw compute shader.
        constexpr AssetId OcclusionCullCompId{0x5FE19B500FD44B52ULL};

        // The core bloom compute shaders (downsample, upsample-accumulate, composite).
        constexpr AssetId BloomDownCompId{0x5B8811BEAC5D9C3BULL};
        constexpr AssetId BloomUpCompId{0x4F28282A720BC9F2ULL};
        constexpr AssetId BloomCompositeCompId{0x533236398AB7654FULL};

        // The Dual Kawase variants of the down/up sweep.
        constexpr AssetId BloomDownKawaseCompId{0xCB1AA796A1E3BBEFULL};
        constexpr AssetId BloomUpKawaseCompId{0x0C269FA0D5F353D2ULL};

        // Linear float HDR format for the lighting target and bloom pyramid.
        // G1 uses the same format as a sampled color target, establishing RGBA16F
        // color-attachment + sampled support on the platform.
        constexpr Format HdrFormat = Format::RGBA16Sfloat;
        constexpr ImageUsage HdrUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;

        // The bloom pyramid's image usage: a compute-written, compute-read mip chain.
        constexpr ImageUsage BloomPyramidUsage = ImageUsage::Storage | ImageUsage::Sampled;

        // The coarsest pyramid level holds a ~8 px edge (2^3) rather than a degenerate
        // 1×1 contributing nothing: the chain stops BloomTileShift levels short.
        constexpr u32 BloomTileShift = 3;

        // Single-channel unorm format for the SSAO target; the renderer builds the
        // SSAO pipeline against this format, and SsaoScenePass owns the image.
        constexpr Format SsaoFormat = Format::R8Unorm;

        // Single-channel float for the hi-Z pyramid; the reduction stores max depth.
        constexpr Format HiZFormat = Format::R32Sfloat;

        // The hi-Z reduction push block: the destination and source mip extents, so a
        // boundary invocation skips out-of-range texels and an odd parent dimension
        // folds its dropped row/column into the max (matches hi_z_reduce.comp).
        struct HiZReducePush
        {
            uvec2 DestExtent;
            uvec2 SourceExtent;
        };

        // The bloom downsample push: the destination mip extent, a level-0 bright-pass
        // flag (1.0 enables bright-pass + Karis), and the soft-knee threshold. Matches
        // bloom_down.comp.
        struct BloomDownPush
        {
            uvec2 DestExtent;
            vec2 SourceScaleUV; // source validExtent/allocExtent (dynamic-resolution sub-rect)
            vec2 SourceMaxUV;   // source (validExtent-0.5)/allocExtent (bilinear-tap clamp)
            f32 BrightPass;
            f32 Threshold;
        };

        // The bloom upsample push: the destination (finer) mip extent, the source sub-rect
        // mapping, and the spread scale. Matches bloom_up.comp.
        struct BloomUpPush
        {
            uvec2 DestExtent;
            vec2 SourceScaleUV;
            vec2 SourceMaxUV;
            f32 Radius;
        };

        // The bloom composite push: the result extent, the source sub-rect mapping (shared by
        // the HDR and bloom-mip-0 inputs, both at mip-0 scale), and the bloom mix. Matches
        // bloom_composite.comp.
        struct BloomCompositePush
        {
            uvec2 DestExtent;
            vec2 SourceScaleUV;
            vec2 SourceMaxUV;
            f32 Intensity;
        };

        // The TAA resolve push block, matching taa_resolve.frag PushConstants: the
        // current/history/depth bindless slots, the shared sampler, the view-constants
        // region, the history-validity flag, and the target extent.
        struct TaaResolvePush
        {
            u32 CurrentTexture;
            u32 HistoryTexture;
            u32 DepthTexture;
            u32 VelocityTexture;
            u32 Sampler;
            u32 ViewConstantsIndex;
            u32 HistoryValid;
            // Pads to an even u32 count so the trailing uint2 lands at its 8-byte-aligned
            // offset, matching the SPIR-V push block layout.
            u32 Pad0;
            uvec2 Extent;
        };

        // The TAA history-copy push block, matching taa_history_copy.frag PushConstants:
        // the resolved source slot and the shared sampler.
        struct TaaCopyPush
        {
            u32 SourceTexture;
            u32 Sampler;
        };

        // The surface push block (vertex stage), matching material.slang PushConstants:
        // FrameBase folds the ring-buffered DrawData region into the candidate id; the
        // view-constants index selects the per-frame set-0 view block the vertex stage
        // multiplies by. Both cull modes push the same block.
        struct SurfacePush
        {
            u32 FrameBase;
            u32 ViewConstantsIndex;
        };

        // Per-draw record indexed by the candidate id; std430-identical to the shader's
        // DrawData. World is the model matrix; the three NormalColumns carry the
        // inverse-transpose of its upper 3×3; MaterialIndex is the frame-folded selector.
        struct GpuDrawData
        {
            mat4 World;
            vec4 NormalColumn0;
            vec4 NormalColumn1;
            vec4 NormalColumn2;
            u32 MaterialIndex;
            u32 PaletteBase;
            u32 PrevPaletteBase;
            u32 Pad2;
            mat4 PrevWorld;
        };

        static_assert(sizeof(GpuDrawData) == 192,
                      "GpuDrawData must match the shader DrawData (192 bytes)");

        // One uploaded camera-frustum survivor; std430-identical to the cull shader's
        // CullCandidate: world AABB plus the indexed-draw args its command needs.
        struct GpuCullCandidate
        {
            vec4 BoundsMin;
            vec4 BoundsMax;
            u32 IndexCount;
            u32 FirstIndex;
            i32 VertexOffset;
            u32 FirstInstance;
        };

        static_assert(sizeof(GpuCullCandidate) == 48,
                      "GpuCullCandidate must match the shader CullCandidate (48 bytes)");

        // VkDrawIndexedIndirectCommand laid out by hand (20 bytes), the stride the
        // indirect geometry pass issues over.
        struct DrawIndexedIndirectCommand
        {
            u32 IndexCount;
            u32 InstanceCount;
            u32 FirstIndex;
            i32 VertexOffset;
            u32 FirstInstance;
        };

        static_assert(sizeof(DrawIndexedIndirectCommand) == 20,
                      "DrawIndexedIndirectCommand must match VkDrawIndexedIndirectCommand");

        // The cull compute push block, matching occlusion_cull.comp PushConstants.
        struct OcclusionCullPush
        {
            mat4 PrevViewProj;
            uvec2 HiZBaseExtent;
            u32 CandidateCount;
            u32 HistoryValid;
            f32 DepthBias;
            u32 FrameBase;
            u32 CountIndex;
        };

        // The deferred-lighting fragment push block: g-buffer bindless slots,
        // view-constants index, and light buffer base + live count.
        // Matches deferred_lighting.frag PushConstants byte-for-byte (eight u32s).
        struct LightingPushConstants
        {
            u32 AlbedoTexture;
            u32 NormalTexture;
            u32 OrmTexture;
            u32 DepthTexture;
            u32 Sampler;
            u32 ViewConstantsIndex;
            u32 LightBase;
            u32 LightCount;
        };

        // The SSAO-enabled lighting variant's push block: base lighting fields plus
        // the AO bindless slot. Matches deferred_lighting_ssao.frag PushConstants
        // byte-for-byte (twelve u32s — three pads reach a 16-byte boundary).
        struct SsaoLightingPushConstants
        {
            u32 AlbedoTexture;
            u32 NormalTexture;
            u32 OrmTexture;
            u32 DepthTexture;
            u32 Sampler;
            u32 ViewConstantsIndex;
            u32 LightBase;
            u32 LightCount;
            u32 SsaoTexture;
            u32 Pad0;
            u32 Pad1;
            u32 Pad2;
        };

        // The per-frame view-constants block (set-0 binding 5): camera/view state only.
        // The directional shadow system rides the set-1 ShadowConstants block.
        // Mirrors material.slang ViewConstants byte-for-byte.
        struct ViewConstantsBlock
        {
            mat4 InvViewProj;    // world-position reconstruction from depth (jittered under TAA)
            vec4 CameraPosition; // xyz; w unused
            mat4 View;           // world → view (the SSAO pass reconstructs view space)
            mat4 Proj;           // view → clip (jittered under TAA)
            mat4 PrevViewProj;   // previous frame's unjittered world → clip (TAA reprojection)
            mat4 CurViewProj;    // this frame's unjittered world → clip (TAA velocity)
            vec4 RenderScaleUV;  // xy this frame's validExtent/allocExtent, zw previous frame's
            vec4 MaxValidUV;     // xy this frame's (validExtent-0.5)/allocExtent, zw previous
        };

        static_assert(sizeof(ViewConstantsBlock) <= BindlessRegistry::ViewConstantsStride,
                      "ViewConstantsBlock must fit one ring-buffered view-constants region");

        // The directional-shadow constants (set 1 binding 2, ring-buffered dynamic
        // uniform). std140: CascadeViewProj is float4x4[MaxCascades] (16-byte aligned
        // elements) and the four splits ride one vec4 (avoiding per-element std140 padding).
        struct ShadowConstantsBlock
        {
            mat4 CascadeViewProj[MaxCascades]; // 256 — tile-remap baked in (for the sample)
            vec4 CascadeSplits;                // 16  — per-cascade view-space far distance
            vec4 ShadowParams; // 16  — x 1/tileRes, y blend-band, z count, w enabled
        };

        static_assert(sizeof(ShadowConstantsBlock) == 288,
                      "ShadowConstantsBlock must be the std140-packed 288-byte block");

        // Set 1 binding 3, ring-buffered dynamic uniform. Separate from ShadowConstantsBlock
        // so the directional block's layout is unchanged when punctual records are added.
        struct PunctualShadowBlock
        {
            PunctualShadowRecord Records[MaxShadowedPunctual];
            vec4 AtlasParams; // x 1/tileRes (per-tile texel size), yzw pad
        };

        static_assert(sizeof(PunctualShadowBlock) == MaxShadowedPunctual * 416 + 16,
                      "PunctualShadowBlock must be the std140-packed record array (416 bytes per "
                      "record) plus the trailing AtlasParams vec4");

        // The punctual shadow atlas tile grid: CubeFaceCount columns × MaxShadowedPunctual
        // rows of PunctualShadowResolution² tiles. A shadowed light's slot s, face f maps
        // to tile (column f, row s) — linear index s·CubeFaceCount + f — so a spot uses
        // tile (0, s) and a point uses the whole of row s.
        constexpr u32 PunctualAtlasColumns = CubeFaceCount;
        constexpr u32 PunctualAtlasRows = MaxShadowedPunctual;

        // Push block shared by all single-target debug blits: a texture + sampler index.
        struct BlitPushConstants
        {
            u32 Texture;
            u32 Sampler;
        };

        // ORM-channel blit push block: ORM texture + sampler + channel selector
        // (0 occlusion / 1 roughness / 2 metallic). Shared by all three ORM debug arms.
        struct OrmBlitPushConstants
        {
            u32 Texture;
            u32 Sampler;
            u32 Channel;
        };

        // One per-candidate draw the geometry pass records, in candidate-slot order.
        // The candidate id (== the slot) reaches the surface vertex stage via the
        // instance attribute fetched at firstInstance; its DrawData record holds the
        // world/normal/material. CPU mode issues a DrawIndexed per slot; GPU mode issues
        // one DrawIndexedIndirect over a mesh group's contiguous command run.
        struct DrawSlot
        {
            const Mesh* SourceMesh;
            u32 IndexCount;
            u32 FirstIndex;
            i32 VertexOffset;
            u32 CandidateId; // == the per-draw DrawData slot and the command firstInstance
        };

        // A contiguous run of candidate slots sharing one source mesh, so the mesh's
        // vertex/index buffers bind once. CPU mode draws each slot; GPU mode issues one
        // vkCmdDrawIndexedIndirect over the run's commands (the culled slots no-op).
        struct DrawGroup
        {
            const Mesh* SourceMesh;
            u32 FirstSlot;
            u32 SlotCount;
        };

        // The per-frame submission plan SceneRenderer fills before each graph replay and
        // the geometry pass reads at record time. Held in SceneRenderer::Internal.
        struct GBufferDrawPlan
        {
            SceneRendererSettings::CullMode Cull = SceneRendererSettings::CullMode::CPU;
            SurfacePush Push;
            Ref<DescriptorSet> DrawDataSet;
            Ref<Buffer> CandidateIdBuffer;
            Ref<Buffer> IndirectBuffer;   // GPU mode only
            u32 IndirectRegionOffset = 0; // byte offset of this frame's command region (GPU)
            // One loaded surface material whose pipeline is bound for the whole pass — every
            // surface material shares the surface pipeline shape, so binding any one suffices.
            // Borrowed: the mesh's resident AssetHandle keeps it alive for this frame.
            const Material* PipelineMaterial = nullptr;
            vector<DrawSlot> Slots;
            vector<DrawGroup> Groups;

            // Skinned draws ride a parallel CPU-direct path after the static draws: the skinned
            // surface pipeline (built from surface_skinned.vert) bound from a representative
            // skinned material, with the per-instance palette bound at set 2. They share the same
            // DrawData buffer (each slot's DrawData.PaletteBase points into the palette).
            const Material* SkinnedPipelineMaterial = nullptr;
            Ref<DescriptorSet> PaletteSet;
            vector<DrawSlot> SkinnedSlots;
            vector<DrawGroup> SkinnedGroups;
        };

        class GBufferScenePass final : public ScenePass
        {
        public:
            GBufferScenePass(Context& context, uvec2 extent, const GBufferDrawPlan* plan,
                             SceneRendererSettings::CullMode cull, ResourceId indirectId)
                : m_Context(context), m_Extent(extent), m_Plan(plan), m_Cull(cull),
                  m_IndirectId(indirectId)
            {
            }

            void Resize(const uvec2 extent) override { m_Extent = extent; }

            void Declare(RenderGraph& graph, const PassIO& io) override
            {
                RenderGraph::PassBuilder builder = graph.AddPass("Scene GBuffer");
                builder
                    .Color({
                        .Resource = io.GBufferAlbedo,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{.R = 0.05f, .G = 0.05f, .B = 0.08f, .A = 1.0f},
                    })
                    .Color({
                        .Resource = io.GBufferNormal,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
                    })
                    .Color({
                        .Resource = io.GBufferOrm,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        // Default occlusion 1 (unoccluded), roughness/metallic/emissive 0
                        // for any background texel; a material overwrites all four.
                        .Clear = ClearColor{.R = 1.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
                    })
                    .Color({
                        // G3 — the per-object motion vector (SV_Target3). The surface fragment
                        // writes it alongside the g-buffer, so motion vectors cost no second
                        // geometry pass. Cleared to zero motion for any background texel; the
                        // TAA resolve falls back to depth reprojection wherever it stays zero.
                        .Resource = io.Velocity,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
                    })
                    .Depth({
                        .Resource = io.GBufferDepth,
                        .Load = LoadOp::Clear,
                        // Stored: the lighting pass reads depth as a texture.
                        .Store = StoreOp::Store,
                        .Clear = ClearDepth{.Depth = 1.0f, .Stencil = 0},
                    });

                // GPU mode reads the cull-written commands as indirect args; declaring the
                // read drives the graph-derived StorageBufferWrite → IndirectRead barrier.
                if (m_Cull == SceneRendererSettings::CullMode::GPU)
                {
                    builder.IndirectRead(m_IndirectId);
                }

                builder.Execute([this](PassContext& inner) { Record(Wrap(inner)); });
            }

        private:
            void Record(const ScenePassContext& ctx) const
            {
                CommandBuffer& cmd = ctx.Cmd();
                const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                const GBufferDrawPlan& plan = *m_Plan;

                // Render into the dynamic-resolution sub-rect; the target stays allocated at the
                // high-water-mark extent (m_Extent), so the consumer upscales the valid region.
                const uvec2 renderExtent = ctx.View().RenderExtent;
                cmd.SetViewport({0, 0}, renderExtent);
                cmd.SetScissor({0, 0}, renderExtent);

                if (plan.Slots.empty() && plan.SkinnedSlots.empty())
                {
                    return;
                }

                // The instance-rate candidate-id buffer (binding 1) is bound once for both the
                // static and skinned passes; each draw's firstInstance selects the candidate id,
                // fetched as the instance attribute that indexes DrawData.
                if (!plan.Slots.empty() || !plan.SkinnedSlots.empty())
                {
                    cmd.GetNative().CommandBuffer.bindVertexBuffers(
                        1, GetVkBuffer(*plan.CandidateIdBuffer), {0});
                }

                if (!plan.Slots.empty())
                {
                    // One surface pipeline drives every static submesh; bind it, set 0 (bindless),
                    // and set 1 (the per-draw DrawData SSBO) once, then push the frame selector.
                    // The pipeline is shared across materials (all surface materials reuse the core
                    // surface.vert + the deferred g-buffer formats), so binding any one binds the
                    // pipeline for the whole static pass (Surface pushes no selector).
                    plan.PipelineMaterial->Bind(cmd);
                    registry.Bind(cmd);

                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {plan.DrawDataSet},
                        .FirstSet = 1,
                        .PipelineBindPoint = PipelineBindPoint::Graphics,
                    });
                    cmd.PushConstants(plan.Push);

                    const Mesh* lastBound = nullptr;
                    for (const DrawGroup& group : plan.Groups)
                    {
                        if (lastBound != group.SourceMesh)
                        {
                            cmd.BindVertexBuffer(group.SourceMesh->GetVertexBuffer());
                            cmd.BindIndexBuffer(group.SourceMesh->GetIndexBuffer());
                            lastBound = group.SourceMesh;
                        }

                        if (plan.Cull == SceneRendererSettings::CullMode::GPU)
                        {
                            // One indirect draw over the group's contiguous command run; culled
                            // slots carry instanceCount 0 and no-op (no GPU-sourced count —
                            // MoltenVK lacks drawIndirectCount).
                            const u64 offset =
                                plan.IndirectRegionOffset + static_cast<u64>(group.FirstSlot) *
                                                                sizeof(DrawIndexedIndirectCommand);
                            cmd.DrawIndexedIndirect(plan.IndirectBuffer, offset, group.SlotCount,
                                                    sizeof(DrawIndexedIndirectCommand));
                        }
                        else
                        {
                            // CPU mode issues a direct DrawIndexed per surviving slot, the
                            // candidate id carried as firstInstance (the same instance path).
                            for (u32 s = 0; s < group.SlotCount; ++s)
                            {
                                const DrawSlot& slot = plan.Slots[group.FirstSlot + s];
                                cmd.DrawIndexed(slot.IndexCount, 1, slot.FirstIndex,
                                                slot.VertexOffset, slot.CandidateId);
                            }
                        }
                    }
                }

                // Skinned draws: the skinned surface pipeline + the palette set (set 2), always
                // CPU-direct (skinned meshes opt out of GPU-driven culling). They share the same
                // DrawData buffer; each slot's DrawData.PaletteBase points into the palette.
                if (!plan.SkinnedSlots.empty() && plan.SkinnedPipelineMaterial != nullptr)
                {
                    plan.SkinnedPipelineMaterial->Bind(cmd);
                    registry.Bind(cmd);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {plan.DrawDataSet},
                        .FirstSet = 1,
                        .PipelineBindPoint = PipelineBindPoint::Graphics,
                    });
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {plan.PaletteSet},
                        .FirstSet = 2,
                        .PipelineBindPoint = PipelineBindPoint::Graphics,
                    });
                    cmd.PushConstants(plan.Push);

                    const Mesh* lastBound = nullptr;
                    for (const DrawGroup& group : plan.SkinnedGroups)
                    {
                        if (lastBound != group.SourceMesh)
                        {
                            cmd.BindVertexBuffer(group.SourceMesh->GetVertexBuffer());
                            cmd.BindIndexBuffer(group.SourceMesh->GetIndexBuffer());
                            lastBound = group.SourceMesh;
                        }
                        for (u32 s = 0; s < group.SlotCount; ++s)
                        {
                            const DrawSlot& slot = plan.SkinnedSlots[group.FirstSlot + s];
                            cmd.DrawIndexed(slot.IndexCount, 1, slot.FirstIndex, slot.VertexOffset,
                                            slot.CandidateId);
                        }
                    }
                }
            }

            Context& m_Context;
            uvec2 m_Extent;
            const GBufferDrawPlan* m_Plan = nullptr;
            SceneRendererSettings::CullMode m_Cull = SceneRendererSettings::CullMode::CPU;
            ResourceId m_IndirectId;
        };

        // Declaring .Sample on each g-buffer id drives the graph-derived attachment →
        // shader-read transitions, including the depth attachment → shader-read barrier.
        class DeferredLightingScenePass final : public ScenePass
        {
        public:
            /// @param useSsao          When true, selects the SSAO-enabled pipeline and push block,
            ///                         and also samples io.Ssao.
            /// @param shadowSet        The dedicated set-1 descriptor set (atlas + comparison sampler
            ///                         + ShadowConstants ring); always valid — the renderer keeps a
            ///                         dummy atlas bound when shadows are off.
            /// @param shadowRingStride Per-frame ShadowConstants region stride; the pass selects the
            ///                         current region via a bind-time dynamic offset.
            /// @param writeToOutput    When true, writes directly to the output target (cascade-debug
            ///                         terminal arm); otherwise writes the HDR target.
            DeferredLightingScenePass(Context& context, Ref<GraphicsPipeline> pipeline,
                                      uvec2 extent, bool useSsao, Ref<DescriptorSet> shadowSet,
                                      u32 shadowRingStride, u32 punctualRingStride,
                                      bool writeToOutput = false)
                : m_Context(context), m_Pipeline(std::move(pipeline)), m_Extent(extent),
                  m_UseSsao(useSsao), m_ShadowSet(std::move(shadowSet)),
                  m_ShadowRingStride(shadowRingStride), m_PunctualRingStride(punctualRingStride),
                  m_WriteToOutput(writeToOutput)
            {
            }

            void Resize(const uvec2 extent) override { m_Extent = extent; }

            void Declare(RenderGraph& graph, const PassIO& io) override
            {
                const TextureHandle albedoHandle = io.AlbedoHandle;
                const TextureHandle normalHandle = io.NormalHandle;
                const TextureHandle ormHandle = io.OrmHandle;
                const TextureHandle depthHandle = io.DepthHandle;
                const TextureHandle ssaoHandle = io.SsaoHandle;
                const SamplerHandle samplerHandle = io.SamplerHandle;
                const bool useSsao = m_UseSsao;
                const Ref<DescriptorSet> shadowSet = m_ShadowSet;
                const u32 shadowRingStride = m_ShadowRingStride;
                const u32 punctualRingStride = m_PunctualRingStride;

                RenderGraph::PassBuilder builder = graph.AddPass("Deferred Lighting");
                builder
                    .Color({
                        .Resource = m_WriteToOutput ? io.Output : io.Hdr,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                    })
                    .Sample(io.GBufferAlbedo)
                    .Sample(io.GBufferNormal)
                    .Sample(io.GBufferOrm)
                    .Sample(io.GBufferDepth);

                // Declaring the shadow/punctual maps sampled drives the graph-derived
                // depth-attachment → shader-read barriers. The atlases reach the
                // lighting shader through set 1 (off bindless); the declarations here
                // are only for barrier derivation.
                if (io.ShadowMap.IsValid())
                {
                    builder.Sample(io.ShadowMap);
                }
                if (io.PunctualShadowMap.IsValid())
                {
                    builder.Sample(io.PunctualShadowMap);
                }

                if (useSsao)
                {
                    builder.Sample(io.Ssao);
                }

                builder.Execute(
                    [this, albedoHandle, normalHandle, ormHandle, depthHandle, ssaoHandle,
                     samplerHandle, useSsao, shadowSet, shadowRingStride,
                     punctualRingStride](PassContext& inner)
                    {
                        const ScenePassContext ctx = Wrap(inner);
                        CommandBuffer& cmd = ctx.Cmd();
                        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();

                        cmd.BindPipeline(m_Pipeline);
                        const uvec2 renderExtent = ctx.View().RenderExtent;
                        cmd.SetViewport({0, 0}, renderExtent);
                        cmd.SetScissor({0, 0}, renderExtent);
                        registry.Bind(cmd);

                        // Bind set 1 (the shadow system: both atlases, comparison sampler, and
                        // both ring-buffered dynamic uniforms). Dynamic offsets select the
                        // current frame-in-flight region, listed in binding order.
                        const u32 frameSlot = registry.GetCurrentViewConstantsIndex();
                        cmd.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {shadowSet},
                            .FirstSet = 1,
                            .PipelineBindPoint = PipelineBindPoint::Graphics,
                            .DynamicOffsets = {frameSlot * shadowRingStride,
                                               frameSlot * punctualRingStride},
                        });

                        if (useSsao)
                        {
                            cmd.PushConstants(SsaoLightingPushConstants{
                                .AlbedoTexture = albedoHandle.Index,
                                .NormalTexture = normalHandle.Index,
                                .OrmTexture = ormHandle.Index,
                                .DepthTexture = depthHandle.Index,
                                .Sampler = samplerHandle.Index,
                                .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                                .LightBase = registry.GetCurrentLightBase(),
                                .LightCount = ctx.View().LightCount,
                                .SsaoTexture = ssaoHandle.Index,
                            });
                        }
                        else
                        {
                            cmd.PushConstants(LightingPushConstants{
                                .AlbedoTexture = albedoHandle.Index,
                                .NormalTexture = normalHandle.Index,
                                .OrmTexture = ormHandle.Index,
                                .DepthTexture = depthHandle.Index,
                                .Sampler = samplerHandle.Index,
                                .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                                .LightBase = registry.GetCurrentLightBase(),
                                .LightCount = ctx.View().LightCount,
                            });
                        }
                        cmd.DrawFullscreenTriangle();
                    });
            }

        private:
            Context& m_Context;
            Ref<GraphicsPipeline> m_Pipeline;
            uvec2 m_Extent;
            bool m_UseSsao = false;
            Ref<DescriptorSet> m_ShadowSet;
            u32 m_ShadowRingStride = 0;
            u32 m_PunctualRingStride = 0;
            bool m_WriteToOutput = false;
        };

        // The declared .Sample on the source id drives the graph-derived attachment → shader-read transition.
        class FullscreenBlitScenePass final : public ScenePass
        {
        public:
            // Which target this blit reads from PassIO. The shadow atlases are off
            // bindless and handled by ShadowBlitScenePass, not this class.
            enum class Source
            {
                Albedo,
                Normal,
                Depth,
                Ao,
                Bloom,
                MotionVectors
            };

            FullscreenBlitScenePass(Context& context, Ref<GraphicsPipeline> pipeline, uvec2 extent,
                                    Source source)
                : m_Context(context), m_Pipeline(std::move(pipeline)), m_Extent(extent),
                  m_Source(source)
            {
            }

            void Resize(const uvec2 extent) override { m_Extent = extent; }

            void Declare(RenderGraph& graph, const PassIO& io) override
            {
                const ResourceId sourceId = SourceId(io);
                const TextureHandle textureHandle = SourceHandle(io);
                const SamplerHandle samplerHandle = io.SamplerHandle;

                graph.AddPass("Debug Blit")
                    .Color({
                        .Resource = io.Output,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                    })
                    .Sample(sourceId)
                    .Execute(
                        [this, textureHandle, samplerHandle](PassContext& inner)
                        {
                            CommandBuffer& cmd = inner.Cmd();
                            cmd.BindPipeline(m_Pipeline);
                            cmd.SetViewport({0, 0}, m_Extent);
                            cmd.SetScissor({0, 0}, m_Extent);
                            m_Context.GetBindlessRegistry().Bind(cmd);
                            cmd.PushConstants(BlitPushConstants{
                                .Texture = textureHandle.Index,
                                .Sampler = samplerHandle.Index,
                            });
                            cmd.DrawFullscreenTriangle();
                        });
            }

        private:
            [[nodiscard]] ResourceId SourceId(const PassIO& io) const
            {
                switch (m_Source)
                {
                case Source::Albedo:
                    return io.GBufferAlbedo;
                case Source::Normal:
                    return io.GBufferNormal;
                case Source::Depth:
                    return io.GBufferDepth;
                case Source::Ao:
                    return io.Ssao;
                case Source::Bloom:
                    return io.BloomMip0;
                case Source::MotionVectors:
                    return io.Velocity;
                }
                VE_ASSERT(false, "FullscreenBlitScenePass: unmapped Source");
            }

            [[nodiscard]] TextureHandle SourceHandle(const PassIO& io) const
            {
                switch (m_Source)
                {
                case Source::Albedo:
                    return io.AlbedoHandle;
                case Source::Normal:
                    return io.NormalHandle;
                case Source::Depth:
                    return io.DepthHandle;
                case Source::Ao:
                    return io.SsaoHandle;
                case Source::Bloom:
                    return io.BloomMip0Handle;
                case Source::MotionVectors:
                    return io.VelocityHandle;
                }
                VE_ASSERT(false, "FullscreenBlitScenePass: unmapped Source");
            }

            Context& m_Context;
            Ref<GraphicsPipeline> m_Pipeline;
            uvec2 m_Extent;
            Source m_Source;
        };

        // Channel: 0 = occlusion, 1 = roughness, 2 = metallic (matches OrmBlitPushConstants::Channel).
        class OrmBlitScenePass final : public ScenePass
        {
        public:
            OrmBlitScenePass(Context& context, Ref<GraphicsPipeline> pipeline, uvec2 extent,
                             u32 channel)
                : m_Context(context), m_Pipeline(std::move(pipeline)), m_Extent(extent),
                  m_Channel(channel)
            {
            }

            void Resize(const uvec2 extent) override { m_Extent = extent; }

            void Declare(RenderGraph& graph, const PassIO& io) override
            {
                const ResourceId ormId = io.GBufferOrm;
                const TextureHandle ormHandle = io.OrmHandle;
                const SamplerHandle samplerHandle = io.SamplerHandle;
                const u32 channel = m_Channel;

                graph.AddPass("ORM Debug Blit")
                    .Color({
                        .Resource = io.Output,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                    })
                    .Sample(ormId)
                    .Execute(
                        [this, ormHandle, samplerHandle, channel](PassContext& inner)
                        {
                            CommandBuffer& cmd = inner.Cmd();
                            cmd.BindPipeline(m_Pipeline);
                            cmd.SetViewport({0, 0}, m_Extent);
                            cmd.SetScissor({0, 0}, m_Extent);
                            m_Context.GetBindlessRegistry().Bind(cmd);
                            cmd.PushConstants(OrmBlitPushConstants{
                                .Texture = ormHandle.Index,
                                .Sampler = samplerHandle.Index,
                                .Channel = channel,
                            });
                            cmd.DrawFullscreenTriangle();
                        });
            }

        private:
            Context& m_Context;
            Ref<GraphicsPipeline> m_Pipeline;
            uvec2 m_Extent;
            u32 m_Channel;
        };

        // Off bindless: reaches the shader through a dedicated set-1 (atlas + ordinary sampler,
        // raw depth). Declares .Sample on the atlas id for the graph-derived barrier.
        class ShadowBlitScenePass final : public ScenePass
        {
        public:
            // Which atlas to visualize: Directional reads io.ShadowMap,
            // Punctual reads io.PunctualShadowMap. The renderer writes the matching
            // atlas view into binding 0 before Rebuild.
            enum class Source
            {
                Directional,
                Punctual
            };

            ShadowBlitScenePass(Context& context, Ref<GraphicsPipeline> pipeline, uvec2 extent,
                                Ref<DescriptorSet> shadowSet, Source source)
                : m_Context(context), m_Pipeline(std::move(pipeline)), m_Extent(extent),
                  m_ShadowSet(std::move(shadowSet)), m_Source(source)
            {
            }

            void Resize(const uvec2 extent) override { m_Extent = extent; }

            void Declare(RenderGraph& graph, const PassIO& io) override
            {
                const Ref<DescriptorSet> shadowSet = m_ShadowSet;
                const ResourceId sampleId =
                    m_Source == Source::Punctual ? io.PunctualShadowMap : io.ShadowMap;

                graph.AddPass("Shadow Debug Blit")
                    .Color({
                        .Resource = io.Output,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                    })
                    .Sample(sampleId)
                    .Execute(
                        [this, shadowSet](PassContext& inner)
                        {
                            CommandBuffer& cmd = inner.Cmd();
                            cmd.BindPipeline(m_Pipeline);
                            cmd.SetViewport({0, 0}, m_Extent);
                            cmd.SetScissor({0, 0}, m_Extent);
                            m_Context.GetBindlessRegistry().Bind(cmd);
                            cmd.BindDescriptorSets(DescriptorSetBindInfo{
                                .Sets = {shadowSet},
                                .FirstSet = 1,
                                .PipelineBindPoint = PipelineBindPoint::Graphics,
                            });
                            cmd.DrawFullscreenTriangle();
                        });
            }

        private:
            Context& m_Context;
            Ref<GraphicsPipeline> m_Pipeline;
            uvec2 m_Extent;
            Ref<DescriptorSet> m_ShadowSet;
            Source m_Source;
        };

        // Temporal anti-aliasing: a resolve pass that reprojects the persisted history
        // against this frame's depth and blends it with the lit color, then a history-copy
        // pass that refreshes the history from the resolved result for next frame.
        //
        // The renderer routes the lighting pass into a separate lit target (litId); this
        // pass reads it as the current frame, writes the resolved color into the HDR target
        // (hdrId) the rest of the chain already samples, and copies that back into historyId.
        class TaaScenePass final : public ScenePass
        {
        public:
            TaaScenePass(Context& context, Ref<GraphicsPipeline> resolvePipeline,
                         Ref<GraphicsPipeline> copyPipeline, ResourceId litId, ResourceId historyId,
                         ResourceId hdrId, ResourceId depthId, ResourceId velocityId,
                         TextureHandle litHandle, TextureHandle historyHandle,
                         TextureHandle velocityHandle, const bool* historyResetPtr, uvec2 extent)
                : m_Context(context), m_ResolvePipeline(std::move(resolvePipeline)),
                  m_CopyPipeline(std::move(copyPipeline)), m_LitId(litId), m_HistoryId(historyId),
                  m_HdrId(hdrId), m_DepthId(depthId), m_VelocityId(velocityId),
                  m_LitHandle(litHandle), m_HistoryHandle(historyHandle),
                  m_VelocityHandle(velocityHandle), m_HistoryResetPtr(historyResetPtr),
                  m_Extent(extent)
            {
            }

            void Resize(const uvec2 extent) override { m_Extent = extent; }

            void Declare(RenderGraph& graph, const PassIO& io) override
            {
                const TextureHandle depthHandle = io.DepthHandle;
                const TextureHandle hdrHandle = io.HdrHandle;
                const SamplerHandle samplerHandle = io.SamplerHandle;

                // Resolve: read the lit color (current), the reprojected history, and depth;
                // write the resolved color into the HDR target downstream passes sample.
                graph.AddPass("TAA Resolve")
                    .Color({
                        .Resource = m_HdrId,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                    })
                    .Sample(m_LitId)
                    .Sample(m_HistoryId)
                    .Sample(m_DepthId)
                    .Sample(m_VelocityId)
                    .Execute(
                        [this, depthHandle, samplerHandle](PassContext& inner)
                        {
                            CommandBuffer& cmd = inner.Cmd();
                            const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                            cmd.BindPipeline(m_ResolvePipeline);
                            const uvec2 renderExtent = Wrap(inner).View().RenderExtent;
                            cmd.SetViewport({0, 0}, renderExtent);
                            cmd.SetScissor({0, 0}, renderExtent);
                            registry.Bind(cmd);
                            cmd.PushConstants(TaaResolvePush{
                                .CurrentTexture = m_LitHandle.Index,
                                .HistoryTexture = m_HistoryHandle.Index,
                                .DepthTexture = depthHandle.Index,
                                .VelocityTexture = m_VelocityHandle.Index,
                                .Sampler = samplerHandle.Index,
                                .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                                .HistoryValid = *m_HistoryResetPtr ? 0u : 1u,
                                .Extent = renderExtent,
                            });
                            cmd.DrawFullscreenTriangle();
                        });

                // History copy: refresh the persisted history from the resolved HDR for the
                // next frame's reprojection. The graph orders it after the resolve's write of
                // the HDR target and after the resolve's read of the history (write-after-read).
                graph.AddPass("TAA History Copy")
                    .Color({
                        .Resource = m_HistoryId,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                    })
                    .Sample(m_HdrId)
                    .Execute(
                        [this, hdrHandle, samplerHandle](PassContext& inner)
                        {
                            CommandBuffer& cmd = inner.Cmd();
                            const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                            cmd.BindPipeline(m_CopyPipeline);
                            const uvec2 renderExtent = Wrap(inner).View().RenderExtent;
                            cmd.SetViewport({0, 0}, renderExtent);
                            cmd.SetScissor({0, 0}, renderExtent);
                            registry.Bind(cmd);
                            cmd.PushConstants(TaaCopyPush{
                                .SourceTexture = hdrHandle.Index,
                                .Sampler = samplerHandle.Index,
                            });
                            cmd.DrawFullscreenTriangle();
                        });
            }

        private:
            Context& m_Context;
            Ref<GraphicsPipeline> m_ResolvePipeline;
            Ref<GraphicsPipeline> m_CopyPipeline;
            ResourceId m_LitId;
            ResourceId m_HistoryId;
            ResourceId m_HdrId;
            ResourceId m_DepthId;
            ResourceId m_VelocityId;
            TextureHandle m_LitHandle;
            TextureHandle m_HistoryHandle;
            TextureHandle m_VelocityHandle;
            // Points at SceneRenderer::m_TaaHistoryReset, read at record time (the graph
            // executes before Execute clears the flag, so it reflects this frame's validity).
            const bool* m_HistoryResetPtr;
            uvec2 m_Extent;
        };
    }

    PostProcessScenePass::PostProcessScenePass(Context& context, AssetHandle<Material> material,
                                               PostProcessInput input, ResourceId output,
                                               Format outputFormat, uvec2 extent)
        : m_Context(context), m_Material(std::move(material)), m_Input(std::move(input)),
          m_Output(output), m_OutputFormat(outputFormat), m_Extent(extent)
    {
    }

    void PostProcessScenePass::BuildPipeline()
    {
        VE_ASSERT(m_Material.IsLoaded(),
                  "PostProcessScenePass: the PostProcess material is not resident");

        const Material& material = *m_Material.Get();
        VE_ASSERT(material.GetDomain() == MaterialDomain::PostProcess,
                  "PostProcessScenePass: material '{}' is not a PostProcess material",
                  material.GetName());

        // The layout (set 0 reserved, selector push range) comes from the material loader;
        // only this color-format-dependent GraphicsPipeline is the pass's to create.
        m_Pipeline = GraphicsPipeline::Create(
            m_Context,
            {
                .Name = fmt::format("PostProcess Pipeline ({})", material.GetName()),
                .ColorAttachments = {{.Format = m_OutputFormat}},
                .PipelineLayout = material.GetPipelineLayout(),
                .ShaderStages =
                    {
                        {.Stage = ShaderStage::Vertex, .Module = material.GetVertexModule()},
                        {.Stage = ShaderStage::Fragment, .Module = material.GetFragmentModule()},
                    },
            });
    }

    void PostProcessScenePass::Declare(RenderGraph& graph, const PassIO& /*io*/)
    {
        if (!m_Pipeline)
        {
            BuildPipeline();
        }

        const PostProcessInput input = m_Input;
        const PostProcessExtraInput extra = m_Extra;
        const bool hasExtra = extra.Texture.IsValid();

        // A two-source pass (bloom composite) declares .Sample on both ids for the
        // graph-derived barriers; single-source passes declare only the primary input.
        RenderGraph::PassBuilder builder = graph.AddPass("PostProcess");
        builder
            .Color({
                .Resource = m_Output,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
            })
            .Sample(input.Source);
        if (hasExtra)
        {
            builder.Sample(extra.Source);
        }

        builder.Execute(
            [this, input, extra, hasExtra](PassContext& inner)
            {
                CommandBuffer& cmd = inner.Cmd();
                // AssetHandle::Get is const; cast away to write the per-frame handle fields.
                auto& material = const_cast<Material&>(*m_Material.Get());

                // Write the live upstream bindless slots; must precede Material::Bind
                // so the pushed selector reads this frame's region.
                material.SetTextureHandle(input.TextureField, input.SourceTexture);
                material.SetSamplerHandle(input.SamplerField, input.Sampler);
                if (hasExtra)
                {
                    material.SetTextureHandle(extra.TextureField, extra.Texture);
                    material.SetSamplerHandle(extra.SamplerField, extra.Sampler);
                }

                // Pipeline must be bound before registry.Bind (Bind uses the active layout).
                // The terminal post pass renders the FULL output and upscales its sub-rect source
                // (the tonemap material samples through ScaledSampleUV), so the output image is
                // valid edge to edge and the compositor needs no sub-rect awareness.
                cmd.BindPipeline(m_Pipeline);
                cmd.SetViewport({0, 0}, m_Extent);
                cmd.SetScissor({0, 0}, m_Extent);
                m_Context.GetBindlessRegistry().Bind(cmd);
                material.Bind(cmd);
                cmd.DrawFullscreenTriangle();
            });
    }

    // Kept out of the header so SceneRenderer.h needs no CompiledGraph definition.
    struct SceneRenderer::Internal
    {
        Unique<CompiledGraph> Graph;
        // The per-frame geometry submission plan PrepareDraws fills before each replay and
        // the geometry pass reads at record time (the pass holds a pointer to it).
        GBufferDrawPlan Plan;
    };

    Unique<SceneRenderer> SceneRenderer::Create(const SceneRendererInfo& info)
    {
        return Unique<SceneRenderer>(new SceneRenderer(info));
    }

    SceneRenderer::SceneRenderer(const SceneRendererInfo& info)
        : m_Context(info.Context), m_Assets(info.Assets), m_OutputFormat(info.OutputFormat),
          m_Extent(info.Extent), m_ValidExtent(info.Extent), m_Settings(info.Settings),
          m_Internal(CreateUnique<Internal>())
    {
        ClampShadowResolutions();
        ResolveActiveCullMode();
        CreateShadowSystem();
        CreatePipelines();

        CreateOutput();
        CreateGBuffer();
        CreateCullResources();
        CreateHdr();
        CreateTaa();
        CreateBloom();
        Rebuild();
    }

    SceneRenderer::~SceneRenderer()
    {
        // Release bindless slots before their images retire.
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_AlbedoHandle);
        bindless.Release(m_NormalHandle);
        bindless.Release(m_OrmHandle);
        bindless.Release(m_DepthHandle);
        bindless.Release(m_HiZSampleHandle);
        bindless.Release(m_HdrHandle);
        bindless.Release(m_LitHandle);
        bindless.Release(m_TaaHistoryHandle);
        bindless.Release(m_VelocityHandle);
        bindless.Release(m_BloomResultHandle);
        bindless.Release(m_BloomMip0Handle);
        bindless.Release(m_SamplerHandle);
    }

    void SceneRenderer::CreatePipelines()
    {
        auto LoadShader = [this](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result =
                m_Assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "SceneRenderer: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };

        const AssetHandle<Veng::Shader> vs = LoadShader(FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> lightingFs =
            LoadShader(DeferredLightingFragId, "deferred-lighting fragment");
        const AssetHandle<Veng::Shader> ssaoLightingFs =
            LoadShader(DeferredLightingSsaoFragId, "deferred-lighting SSAO fragment");
        const AssetHandle<Veng::Shader> cascadeDebugFs =
            LoadShader(DeferredLightingCascadesFragId, "deferred-lighting cascade-debug fragment");
        const AssetHandle<Veng::Shader> ssaoFs = LoadShader(SsaoFragId, "SSAO fragment");
        const AssetHandle<Veng::Shader> taaResolveFs =
            LoadShader(TaaResolveFragId, "TAA resolve fragment");
        const AssetHandle<Veng::Shader> taaCopyFs =
            LoadShader(TaaHistoryCopyFragId, "TAA history-copy fragment");
        const AssetHandle<Veng::Shader> albedoBlitFs =
            LoadShader(AlbedoBlitFragId, "albedo-blit fragment");
        const AssetHandle<Veng::Shader> normalBlitFs =
            LoadShader(NormalBlitFragId, "normal-blit fragment");
        const AssetHandle<Veng::Shader> depthBlitFs =
            LoadShader(DepthBlitFragId, "depth-blit fragment");
        const AssetHandle<Veng::Shader> ormBlitFs = LoadShader(OrmBlitFragId, "ORM-blit fragment");
        const AssetHandle<Veng::Shader> aoBlitFs = LoadShader(AoBlitFragId, "AO-blit fragment");
        const AssetHandle<Veng::Shader> motionBlitFs =
            LoadShader(MotionBlitFragId, "motion-vector-blit fragment");
        const AssetHandle<Veng::Shader> shadowBlitFs =
            LoadShader(ShadowBlitFragId, "shadow-blit fragment");

        // Builds a fullscreen pipeline (shared vertex stage) over a layout, naming the
        // color-target format the pass writes.
        auto MakePipeline = [&](const char* name, const Ref<PipelineLayout>& layout,
                                const AssetHandle<Veng::Shader>& fs,
                                const Format format) -> Ref<GraphicsPipeline>
        {
            return GraphicsPipeline::Create(
                m_Context, {
                               .Name = name,
                               .ColorAttachments = {{.Format = format}},
                               .PipelineLayout = layout,
                               .ShaderStages =
                                   {
                                       {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                       {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                                   },
                           });
        };

        // Both lighting layouts carry set 1 (the shadow system: atlas + immutable
        // comparison sampler + ShadowConstants dynamic uniform). Set 0 is the reserved
        // registry slot prepended by PipelineLayout, so set 1 is at descriptor-set index 1.
        m_LightingLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Lighting Layout",
                           .DescriptorSetLayouts = {m_ShadowSetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<LightingPushConstants>(
                               ShaderStage::Fragment)},
                       });
        m_LightingPipeline = MakePipeline("SceneRenderer Deferred Lighting Pipeline",
                                          m_LightingLayout, lightingFs, HdrFormat);

        // SSAO-enabled lighting variant: wider push block (adds the AO slot) and
        // the AO-fold fragment shader. Same set-1 shadow layout.
        m_SsaoLightingLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer SSAO Lighting Layout",
                           .DescriptorSetLayouts = {m_ShadowSetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<SsaoLightingPushConstants>(
                               ShaderStage::Fragment)},
                       });
        m_SsaoLightingPipeline = MakePipeline("SceneRenderer Deferred Lighting SSAO Pipeline",
                                              m_SsaoLightingLayout, ssaoLightingFs, HdrFormat);

        // Cascade-debug variant reuses the plain lighting layout but writes the output
        // format directly — a terminal debug arm with no tonemap tail.
        m_CascadeDebugPipeline = MakePipeline("SceneRenderer Cascade Debug Pipeline",
                                              m_LightingLayout, cascadeDebugFs, m_OutputFormat);

        // Push block is eight u32s (g-buffer slots + extent); Size = 32 matches SsaoPushConstants.
        m_SsaoLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer SSAO Layout",
                           .PushConstantRanges = {PushConstantRange{
                               .Stages = ShaderStage::Fragment, .Offset = 0, .Size = 32}},
                       });
        m_SsaoPipeline =
            MakePipeline("SceneRenderer SSAO Pipeline", m_SsaoLayout, ssaoFs, SsaoFormat);

        // TAA resolve writes the HDR target (the resolved color the rest of the chain reads);
        // its push carries the bindless slots, view-constants region, history flag, and extent.
        m_TaaResolveLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer TAA Resolve Layout",
                           .PushConstantRanges = {PushConstantRange::Of<TaaResolvePush>(
                               ShaderStage::Fragment)},
                       });
        m_TaaResolvePipeline = MakePipeline("SceneRenderer TAA Resolve Pipeline",
                                            m_TaaResolveLayout, taaResolveFs, HdrFormat);

        // TAA history-copy writes the persisted history target (HDR, unclamped).
        m_TaaCopyLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer TAA History Copy Layout",
                .PushConstantRanges = {PushConstantRange::Of<TaaCopyPush>(ShaderStage::Fragment)},
            });
        m_TaaCopyPipeline = MakePipeline("SceneRenderer TAA History Copy Pipeline", m_TaaCopyLayout,
                                         taaCopyFs, HdrFormat);

        // Loaded resident so the PostProcessScenePass builds its pipeline against the output format.
        const AssetResult<AssetHandle<Material>> tonemap =
            m_Assets.LoadSync<Material>(TonemapMaterialId);
        VE_ASSERT(tonemap.has_value(), "SceneRenderer: tonemap material load failed: {}",
                  tonemap.error().Detail);
        m_TonemapMaterial = *tonemap;

        // The g-buffer debug blits share the BlitPushConstants layout; only the
        // fragment shader differs.
        const PushConstantRange blitRange =
            PushConstantRange::Of<BlitPushConstants>(ShaderStage::Fragment);

        m_AlbedoBlitLayout =
            PipelineLayout::Create(m_Context, {
                                                  .Name = "SceneRenderer Albedo Blit Layout",
                                                  .PushConstantRanges = {blitRange},
                                              });
        m_AlbedoBlitPipeline = MakePipeline("SceneRenderer Albedo Blit Pipeline",
                                            m_AlbedoBlitLayout, albedoBlitFs, m_OutputFormat);

        m_NormalBlitLayout =
            PipelineLayout::Create(m_Context, {
                                                  .Name = "SceneRenderer Normal Blit Layout",
                                                  .PushConstantRanges = {blitRange},
                                              });
        m_NormalBlitPipeline = MakePipeline("SceneRenderer Normal Blit Pipeline",
                                            m_NormalBlitLayout, normalBlitFs, m_OutputFormat);

        m_DepthBlitLayout =
            PipelineLayout::Create(m_Context, {
                                                  .Name = "SceneRenderer Depth Blit Layout",
                                                  .PushConstantRanges = {blitRange},
                                              });
        m_DepthBlitPipeline = MakePipeline("SceneRenderer Depth Blit Pipeline", m_DepthBlitLayout,
                                           depthBlitFs, m_OutputFormat);

        // AO blit: same push shape as the g-buffer blits.
        m_AoBlitLayout =
            PipelineLayout::Create(m_Context, {
                                                  .Name = "SceneRenderer AO Blit Layout",
                                                  .PushConstantRanges = {blitRange},
                                              });
        m_AoBlitPipeline = MakePipeline("SceneRenderer AO Blit Pipeline", m_AoBlitLayout, aoBlitFs,
                                        m_OutputFormat);

        // Motion-vector blit: samples the velocity target through the same texture+sampler
        // push as the g-buffer blits.
        m_MotionBlitLayout =
            PipelineLayout::Create(m_Context, {
                                                  .Name = "SceneRenderer Motion Blit Layout",
                                                  .PushConstantRanges = {blitRange},
                                              });
        m_MotionBlitPipeline = MakePipeline("SceneRenderer Motion Blit Pipeline",
                                            m_MotionBlitLayout, motionBlitFs, m_OutputFormat);

        // Shadow blit reads raw depth through a dedicated set 1, not bindless,
        // so its layout carries that set and no push block.
        m_ShadowBlitLayout =
            PipelineLayout::Create(m_Context, {
                                                  .Name = "SceneRenderer Shadow Blit Layout",
                                                  .DescriptorSetLayouts = {m_ShadowBlitSetLayout},
                                              });
        m_ShadowBlitPipeline = MakePipeline("SceneRenderer Shadow Blit Pipeline",
                                            m_ShadowBlitLayout, shadowBlitFs, m_OutputFormat);

        m_OrmBlitLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer ORM Blit Layout",
                           .PushConstantRanges = {PushConstantRange::Of<OrmBlitPushConstants>(
                               ShaderStage::Fragment)},
                       });
        m_OrmBlitPipeline = MakePipeline("SceneRenderer ORM Blit Pipeline", m_OrmBlitLayout,
                                         ormBlitFs, m_OutputFormat);

        // The hi-Z reduction compute pipeline. Set 1 (binding 0 sampled source, binding 1
        // storage dest) is off bindless — a closed producer→consumer reduction needs no
        // global registration, and a dedicated set sidesteps the set-0 storage-image
        // argument-buffer path on MoltenVK.
        const AssetHandle<Veng::Shader> hiZReduceCs =
            LoadShader(HiZReduceCompId, "hi-Z reduce compute");
        m_HiZReduceSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer HiZ Reduce Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 1,
                                    .Type = DescriptorType::StorageImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                               },
                       });
        m_HiZReduceLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer HiZ Reduce Layout",
                .DescriptorSetLayouts = {m_HiZReduceSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<HiZReducePush>(ShaderStage::Compute)},
            });
        m_HiZReducePipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer HiZ Reduce Pipeline",
                .PipelineLayout = m_HiZReduceLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = hiZReduceCs.Get()->Module},
            });

        // The bloom compute pipelines. Set 1 is off bindless (the closed bloom chain needs
        // no global registration, and a dedicated set sidesteps the set-0 storage-image
        // argument-buffer path on MoltenVK). Down/up share one set layout (sampled source +
        // linear sampler + storage dest); composite needs a distinct one (two sampled inputs).
        const AssetHandle<Veng::Shader> bloomDownCs =
            LoadShader(BloomDownCompId, "bloom downsample");
        const AssetHandle<Veng::Shader> bloomUpCs = LoadShader(BloomUpCompId, "bloom upsample");
        const AssetHandle<Veng::Shader> bloomDownKawaseCs =
            LoadShader(BloomDownKawaseCompId, "bloom downsample (Kawase)");
        const AssetHandle<Veng::Shader> bloomUpKawaseCs =
            LoadShader(BloomUpKawaseCompId, "bloom upsample (Kawase)");
        const AssetHandle<Veng::Shader> bloomCompositeCs =
            LoadShader(BloomCompositeCompId, "bloom composite");

        m_BloomDownUpSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Bloom DownUp Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 1,
                                    .Type = DescriptorType::Sampler,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 2,
                                    .Type = DescriptorType::StorageImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                               },
                       });
        m_BloomCompositeSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Bloom Composite Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 1,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 2,
                                    .Type = DescriptorType::Sampler,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 3,
                                    .Type = DescriptorType::StorageImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                               },
                       });

        m_BloomDownUpLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer Bloom DownUp Layout",
                .DescriptorSetLayouts = {m_BloomDownUpSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<BloomDownPush>(ShaderStage::Compute)},
            });
        m_BloomCompositeLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Bloom Composite Layout",
                           .DescriptorSetLayouts = {m_BloomCompositeSetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<BloomCompositePush>(
                               ShaderStage::Compute)},
                       });

        m_BloomDownPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Bloom Down Pipeline",
                .PipelineLayout = m_BloomDownUpLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = bloomDownCs.Get()->Module},
            });
        m_BloomUpPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Bloom Up Pipeline",
                .PipelineLayout = m_BloomDownUpLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = bloomUpCs.Get()->Module},
            });
        m_BloomDownKawasePipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "SceneRenderer Bloom Down Kawase Pipeline",
                           .PipelineLayout = m_BloomDownUpLayout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = bloomDownKawaseCs.Get()->Module},
                       });
        m_BloomUpKawasePipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "SceneRenderer Bloom Up Kawase Pipeline",
                           .PipelineLayout = m_BloomDownUpLayout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = bloomUpKawaseCs.Get()->Module},
                       });
        m_BloomCompositePipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "SceneRenderer Bloom Composite Pipeline",
                           .PipelineLayout = m_BloomCompositeLayout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = bloomCompositeCs.Get()->Module},
                       });
    }

    void SceneRenderer::CreateShadowSystem()
    {
        m_FramesInFlight = m_Context.GetMaxFramesInFlight();

        // Immutable comparison sampler for hardware SampleCmp: LESS-or-equal, linear
        // filter for the hardware 2×2 PCF. The MoltenVK argument-buffer restriction
        // applies only to set-0 bindless arrays; a dedicated set 1 sampler is fine.
        m_ComparisonSampler =
            Sampler::Create(m_Context, {
                                           .Name = "SceneRenderer Shadow Comparison Sampler",
                                           .MagFilter = Filter::Linear,
                                           .MinFilter = Filter::Linear,
                                           .MipmapMode = MipmapMode::Nearest,
                                           .AddressModeU = AddressMode::ClampToEdge,
                                           .AddressModeV = AddressMode::ClampToEdge,
                                           .AddressModeW = AddressMode::ClampToEdge,
                                           .AnisotropyEnabled = false,
                                           .CompareEnable = true,
                                           .CompareOp = CompareOp::LessOrEqual,
                                           .BorderColor = BorderColor::OpaqueWhite,
                                       });

        // Set 1 — the shadow system:
        //   0: directional cascade atlas (SampledImage)
        //   1: shared immutable comparison sampler (baked into the layout)
        //   2: ShadowConstants dynamic uniform (ring-buffered)
        //   3: PunctualShadowBlock dynamic uniform (ring-buffered)
        //   4: punctual shadow atlas (SampledImage)
        // The comparison sampler is shared across cascade and punctual tiles; binding 1
        // is immutable, so descriptor writes supply only bindings 0, 4 and buffers 2, 3.
        m_ShadowSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Shadow Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                                   {.Binding = 1,
                                    .Type = DescriptorType::Sampler,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment,
                                    .ImmutableSamplers = {m_ComparisonSampler}},
                                   {.Binding = 2,
                                    .Type = DescriptorType::UniformBufferDynamic,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                                   {.Binding = 3,
                                    .Type = DescriptorType::UniformBufferDynamic,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                                   {.Binding = 4,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                               },
                       });
        m_ShadowSet = DescriptorSet::Create(m_Context, {
                                                           .Name = "SceneRenderer Shadow Set",
                                                           .Layout = m_ShadowSetLayout,
                                                       });

        // Debug shadow-blit set: atlas (binding 0) + ordinary sampler (binding 1) for raw depth.
        m_ShadowBlitSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Shadow Blit Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                                   {.Binding = 1,
                                    .Type = DescriptorType::Sampler,
                                    .Count = 1,
                                    .Stages = ShaderStage::Fragment},
                               },
                       });
        m_ShadowBlitSet =
            DescriptorSet::Create(m_Context, {
                                                 .Name = "SceneRenderer Shadow Blit Set",
                                                 .Layout = m_ShadowBlitSetLayout,
                                             });
        // Ordinary clamp sampler for raw-depth debug reads; the descriptor set retains it.
        const Ref<Sampler> blitSampler =
            Sampler::Create(m_Context, {
                                           .Name = "SceneRenderer Shadow Blit Sampler",
                                           .MagFilter = Filter::Nearest,
                                           .MinFilter = Filter::Nearest,
                                           .MipmapMode = MipmapMode::Nearest,
                                           .AddressModeU = AddressMode::ClampToEdge,
                                           .AddressModeV = AddressMode::ClampToEdge,
                                           .AddressModeW = AddressMode::ClampToEdge,
                                           .AnisotropyEnabled = false,
                                       });
        m_ShadowBlitSet->Write(1, blitSampler);

        // 1×1 D32 dummy atlas cleared to depth = 1 (full visibility), bound when no
        // shadow pass is wired so the layout is always satisfied. Transitioned to
        // ShaderReadOnly immediately so the lighting pass samples a valid layout even
        // when it does not declare .Sample on it.
        m_DummyShadowImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer Dummy Shadow",
                                         .Extent = {1, 1, 1},
                                         .Format = Format::D32Sfloat,
                                         .Usage = ImageUsage::DepthAttachment | ImageUsage::Sampled,
                                     });
        m_DummyShadowView =
            ImageView::Create(m_Context, {
                                             .Name = "SceneRenderer Dummy Shadow View",
                                             .Image = m_DummyShadowImage,
                                         });
        m_Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                RenderGraph graph(m_Context);
                const ResourceId target = graph.Import("Dummy Shadow");
                graph.AddPass("Clear Dummy Shadow")
                    .Depth({
                        .Resource = target,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearDepth{.Depth = 1.0f, .Stencil = 0},
                    })
                    .Execute([](PassContext&) {});
                const RenderGraph::ImportBinding binding{.Id = target, .View = m_DummyShadowView};
                graph.Compile()->Execute(cmd, {&binding, 1});
                cmd.PrepareForAccess(m_DummyShadowView, AccessKind::Sample);
            });

        // ShadowConstants ring: framesInFlight regions, each aligned to
        // minUniformBufferOffsetAlignment. Dynamic offset at bind time = frame * stride.
        const u64 minAlign =
            GetVkPhysicalDevice(m_Context).getProperties().limits.minUniformBufferOffsetAlignment;
        const u64 blockSize = sizeof(ShadowConstantsBlock);
        const u64 alignment = minAlign == 0 ? 1 : minAlign;
        m_ShadowRingStride =
            static_cast<u32>(((blockSize + alignment - 1) / alignment) * alignment);
        VE_ASSERT(m_ShadowRingStride % alignment == 0,
                  "ShadowConstants ring stride {} is not a multiple of "
                  "minUniformBufferOffsetAlignment {}",
                  m_ShadowRingStride, alignment);

        m_ShadowConstantsBuffer = Buffer::Create(
            m_Context, {
                           .Name = "SceneRenderer ShadowConstants",
                           .Size = static_cast<u64>(m_ShadowRingStride) * m_FramesInFlight,
                           .Usage = BufferUsage::Uniform,
                           .HostMapped = true,
                       });

        // Zero all regions: w = 0 in ShadowParams means shadows disabled.
        std::memset(m_ShadowConstantsBuffer->GetMappedData(), 0,
                    static_cast<usize>(m_ShadowRingStride) * m_FramesInFlight);

        m_ShadowSet->Write(2, m_ShadowConstantsBuffer, 0, sizeof(ShadowConstantsBlock));

        // PunctualShadowBlock ring: same alignment and frame*stride dynamic offset as
        // the ShadowConstants ring.
        const u64 punctualBlockSize = sizeof(PunctualShadowBlock);
        m_PunctualRingStride =
            static_cast<u32>(((punctualBlockSize + alignment - 1) / alignment) * alignment);
        VE_ASSERT(m_PunctualRingStride % alignment == 0,
                  "PunctualShadowBlock ring stride {} is not a multiple of "
                  "minUniformBufferOffsetAlignment {}",
                  m_PunctualRingStride, alignment);

        m_PunctualShadowBuffer = Buffer::Create(
            m_Context, {
                           .Name = "SceneRenderer PunctualShadows",
                           .Size = static_cast<u64>(m_PunctualRingStride) * m_FramesInFlight,
                           .Usage = BufferUsage::Uniform,
                           .HostMapped = true,
                       });

        // Zero all regions: Params.x type = 0 means "no map", so all lights read full visibility.
        std::memset(m_PunctualShadowBuffer->GetMappedData(), 0,
                    static_cast<usize>(m_PunctualRingStride) * m_FramesInFlight);

        m_ShadowSet->Write(3, m_PunctualShadowBuffer, 0, sizeof(PunctualShadowBlock));

        CreatePunctualShadowAtlas(); // allocates, clears, and writes binding 4

        WriteShadowAtlasBinding(
            m_DummyShadowView); // Rebuild overwrites binding 0 when shadows are on
    }

    void SceneRenderer::CreatePunctualShadowAtlas()
    {
        // 2D depth atlas of CubeFaceCount × MaxShadowedPunctual tiles.
        const u32 res = m_Settings.PunctualShadowResolution;
        const uvec2 atlasExtent{PunctualAtlasColumns * res, PunctualAtlasRows * res};

        m_PunctualShadowImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer Punctual Shadow Atlas",
                                         .Extent = {atlasExtent.x, atlasExtent.y, 1},
                                         .Format = Format::D32Sfloat,
                                         .Usage = ImageUsage::DepthAttachment | ImageUsage::Sampled,
                                     });
        m_PunctualShadowView =
            ImageView::Create(m_Context, {
                                             .Name = "SceneRenderer Punctual Shadow Atlas View",
                                             .Image = m_PunctualShadowImage,
                                         });

        // Clear to depth = 1 (full visibility) and transition to ShaderReadOnly so
        // binding 4 is in a valid sampleable layout before the punctual pass runs.
        m_Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                RenderGraph graph(m_Context);
                const ResourceId target = graph.Import("Clear Punctual Atlas");
                graph.AddPass("Clear Punctual Shadow Atlas")
                    .Depth({
                        .Resource = target,
                        .Load = LoadOp::Clear,
                        .Store = StoreOp::Store,
                        .Clear = ClearDepth{.Depth = 1.0f, .Stencil = 0},
                    })
                    .Execute([](PassContext&) {});
                const RenderGraph::ImportBinding binding{.Id = target,
                                                         .View = m_PunctualShadowView};
                graph.Compile()->Execute(cmd, {&binding, 1});
                cmd.PrepareForAccess(m_PunctualShadowView, AccessKind::Sample);
            });

        m_ShadowSet->Write(4, m_PunctualShadowView);
    }

    void SceneRenderer::WriteShadowAtlasBinding(const Ref<ImageView>& atlasView)
    {
        m_ShadowSet->Write(0, atlasView);
        m_ShadowBlitSet->Write(0, atlasView);
    }

    void SceneRenderer::CreateOutput()
    {
        // TransferSrc for the smoke path Download(); Sampled for the composite consumer.
        // Single-copy: the consumer samples GetOutput() in the same frame it is written.
        m_OutputImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer Output",
                                         .Extent = {m_Extent.x, m_Extent.y, 1},
                                         .Format = m_OutputFormat,
                                         .Usage = ImageUsage::ColorAttachment |
                                                  ImageUsage::Sampled | ImageUsage::TransferSrc,
                                     });

        m_OutputView = ImageView::Create(m_Context, {
                                                        .Name = "SceneRenderer Output View",
                                                        .Image = m_OutputImage,
                                                    });
    }

    void SceneRenderer::CreateGBuffer()
    {
        // Renderer-owned and Imported (not a graph transient — sampled downstream through bindless).
        // Releasing old slots defers through the same per-frame window as the images retire.
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_AlbedoHandle);
        bindless.Release(m_NormalHandle);
        bindless.Release(m_OrmHandle);
        bindless.Release(m_DepthHandle);
        bindless.Release(m_VelocityHandle);

        m_AlbedoImage = Image::Create(m_Context, {
                                                     .Name = "SceneRenderer GBuffer Albedo",
                                                     .Extent = {m_Extent.x, m_Extent.y, 1},
                                                     .Format = GBuffer::AlbedoFormat,
                                                     .Usage = GBuffer::ColorUsage,
                                                 });
        m_AlbedoView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer GBuffer Albedo View", .Image = m_AlbedoImage});

        m_NormalImage = Image::Create(m_Context, {
                                                     .Name = "SceneRenderer GBuffer Normal",
                                                     .Extent = {m_Extent.x, m_Extent.y, 1},
                                                     .Format = GBuffer::NormalFormat,
                                                     .Usage = GBuffer::ColorUsage,
                                                 });
        m_NormalView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer GBuffer Normal View", .Image = m_NormalImage});

        m_OrmImage = Image::Create(m_Context, {
                                                  .Name = "SceneRenderer GBuffer ORM",
                                                  .Extent = {m_Extent.x, m_Extent.y, 1},
                                                  .Format = GBuffer::ORMFormat,
                                                  .Usage = GBuffer::ColorUsage,
                                              });
        m_OrmView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer GBuffer ORM View", .Image = m_OrmImage});

        m_DepthImage = Image::Create(m_Context, {
                                                    .Name = "SceneRenderer GBuffer Depth",
                                                    .Extent = {m_Extent.x, m_Extent.y, 1},
                                                    .Format = GBuffer::DepthFormat,
                                                    .Usage = GBuffer::DepthUsage,
                                                });
        m_DepthView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer GBuffer Depth View", .Image = m_DepthImage});

        // G3 — the per-object screen-space motion vector. A g-buffer channel like the others:
        // the surface pass writes it as SV_Target3 every frame (the TAA resolve and the
        // MotionVectors debug blit read it), so it is always allocated, not TAA-gated.
        m_VelocityImage = Image::Create(m_Context, {
                                                       .Name = "SceneRenderer GBuffer Velocity",
                                                       .Extent = {m_Extent.x, m_Extent.y, 1},
                                                       .Format = GBuffer::VelocityFormat,
                                                       .Usage = GBuffer::ColorUsage,
                                                   });
        m_VelocityView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer GBuffer Velocity View", .Image = m_VelocityImage});

        if (!m_Sampler)
        {
            m_Sampler = Sampler::Create(m_Context, {
                                                       .Name = "SceneRenderer GBuffer Sampler",
                                                       .AddressModeU = AddressMode::ClampToEdge,
                                                       .AddressModeV = AddressMode::ClampToEdge,
                                                       .AddressModeW = AddressMode::ClampToEdge,
                                                   });
            m_SamplerHandle = bindless.Register(m_Sampler);
        }

        m_AlbedoHandle = bindless.Register(m_AlbedoView);
        m_NormalHandle = bindless.Register(m_NormalView);
        m_OrmHandle = bindless.Register(m_OrmView);
        m_DepthHandle = bindless.Register(m_DepthView);
        m_VelocityHandle = bindless.Register(m_VelocityView);

        CreateHiZ();
    }

    void SceneRenderer::CreateHiZ()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_HiZSampleHandle);

        // A full mip chain over the depth extent: floor(log2(max(w,h))) + 1 levels.
        const u32 maxDim = std::max(m_Extent.x, m_Extent.y);
        const u32 mipCount = maxDim == 0 ? 1 : (std::bit_width(maxDim));

        m_HiZImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer HiZ",
                                         .Extent = {m_Extent.x, m_Extent.y, 1},
                                         .MipLevels = mipCount,
                                         .Format = HiZFormat,
                                         .Usage = ImageUsage::Storage | ImageUsage::Sampled,
                                     });

        // One single-mip storage view per level (the reduction writes each), plus a
        // whole-chain sampled view for the occlusion test.
        m_HiZMips.clear();
        m_HiZMips.reserve(mipCount);
        for (u32 level = 0; level < mipCount; level++)
        {
            m_HiZMips.push_back(ImageView::Create(
                m_Context, {
                               .Name = fmt::format("SceneRenderer HiZ Mip {} View", level),
                               .Image = m_HiZImage,
                               .BaseMipLevel = level,
                               .MipLevels = 1,
                           }));
        }
        m_HiZSampleView = ImageView::Create(m_Context, {
                                                           .Name = "SceneRenderer HiZ Sample View",
                                                           .Image = m_HiZImage,
                                                           .MipLevels = mipCount,
                                                       });
        m_HiZSampleHandle = bindless.Register(m_HiZSampleView);

        // Per-mip reduction descriptor sets: set k binds mip k's source (the depth
        // target for k=0, hi-Z mip k-1 otherwise) and mip k's destination storage view.
        m_HiZReduceSets.clear();
        m_HiZReduceSets.reserve(mipCount);
        for (u32 level = 0; level < mipCount; level++)
        {
            Ref<DescriptorSet> set = DescriptorSet::Create(
                m_Context, {
                               .Name = fmt::format("SceneRenderer HiZ Reduce Set {}", level),
                               .Layout = m_HiZReduceSetLayout,
                           });
            const Ref<ImageView>& source = level == 0 ? m_DepthView : m_HiZMips[level - 1];
            set->Write(0, source);
            set->Write(1, m_HiZMips[level]);
            m_HiZReduceSets.push_back(std::move(set));
        }

        // The freshly created pyramid carries no last-frame depth; the next Execute must
        // skip occlusion rather than test against an undefined/stale chain.
        m_HiZHistoryReset = true;

        // The cull set samples the pyramid through binding 0; rewrite it whenever the
        // pyramid is recreated (Resize/Configure). Skipped on the first CreateHiZ, before
        // CreateCullResources has made the set.
        if (m_CullSet)
        {
            m_CullSet->Write(0, m_HiZSampleView);
        }
    }

    void SceneRenderer::CreateCullResources()
    {
        // The per-draw DrawData SSBO drives both cull modes' buffer-indexed draw. Host-visible,
        // ring-buffered for frames-in-flight; the surface vertex stage indexes it by the pushed
        // FrameBase folded with the candidate id.
        const u64 drawDataRegion = static_cast<u64>(MaxCullCandidates) * sizeof(GpuDrawData);
        m_DrawDataBuffer = Buffer::Create(m_Context, {
                                                         .Name = "SceneRenderer DrawData",
                                                         .Size = drawDataRegion * m_FramesInFlight,
                                                         .Usage = BufferUsage::Storage,
                                                         .HostMapped = true,
                                                     });

        // Stage flags must match the surface pipeline's reflected set-1 layout exactly for
        // descriptor-set compatibility. The shared material header declares g_DrawData in
        // both stages (the fragment includes it even though only the vertex stage reads it),
        // so the cooker reflects it Vertex | Fragment — match that here.
        m_DrawDataSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer DrawData Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageBuffer,
                                         .Count = 1,
                                         .Stages = ShaderStage::Vertex | ShaderStage::Fragment}},
                       });
        m_DrawDataSet = DescriptorSet::Create(m_Context, {
                                                             .Name = "SceneRenderer DrawData Set",
                                                             .Layout = m_DrawDataSetLayout,
                                                         });
        m_DrawDataSet->Write(0, m_DrawDataBuffer);

        // The per-instance skinning palette drives skinned draws. Host-visible, ring-buffered;
        // a skinned draw's DrawData.PaletteBase indexes it directly. Vertex-stage only — the
        // skinned vertex shaders declare g_Palette in the vertex stage alone, so the reflected
        // set layout (set 2 for the surface pipeline, set 1 for the shadow pipeline) is Vertex.
        const u64 paletteRegion = static_cast<u64>(MaxSkinningMatricesPerFrame) * sizeof(mat4);
        m_PaletteBuffer = Buffer::Create(m_Context, {
                                                        .Name = "SceneRenderer Skinning Palette",
                                                        .Size = paletteRegion * m_FramesInFlight,
                                                        .Usage = BufferUsage::Storage,
                                                        .HostMapped = true,
                                                    });
        m_PaletteSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Palette Set Layout",
                           .Bindings = {{.Binding = 0,
                                         .Type = DescriptorType::StorageBuffer,
                                         .Count = 1,
                                         .Stages = ShaderStage::Vertex}},
                       });
        m_PaletteSet = DescriptorSet::Create(m_Context, {
                                                            .Name = "SceneRenderer Palette Set",
                                                            .Layout = m_PaletteSetLayout,
                                                        });
        m_PaletteSet->Write(0, m_PaletteBuffer);

        // The identity candidate-id buffer bound to vertex binding 1 (instance rate): element k
        // holds k, so a draw's firstInstance = candidateId fetches candidateId as the attribute.
        vector<u32> identity(MaxCullCandidates);
        for (u32 i = 0; i < MaxCullCandidates; ++i)
        {
            identity[i] = i;
        }
        m_CandidateIdBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "SceneRenderer Candidate Ids",
                                          .Size = identity.size() * sizeof(u32),
                                          .Usage = BufferUsage::Vertex | BufferUsage::TransferDst,
                                      });
        m_CandidateIdBuffer->UploadSync(std::span<const u8>(
            reinterpret_cast<const u8*>(identity.data()), identity.size() * sizeof(u32)));

        // The GPU cull path's buffers + pipeline build only where the device supports it.
        if (!m_Context.IsGpuDrivenCullingSupported())
        {
            return;
        }

        const u64 candidateRegion = static_cast<u64>(MaxCullCandidates) * sizeof(GpuCullCandidate);
        m_CullCandidateBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "SceneRenderer Cull Candidates",
                                          .Size = candidateRegion * m_FramesInFlight,
                                          .Usage = BufferUsage::Storage,
                                          .HostMapped = true,
                                      });

        const u64 indirectRegion =
            static_cast<u64>(MaxCullCandidates) * sizeof(DrawIndexedIndirectCommand);
        m_IndirectBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "SceneRenderer Indirect Commands",
                                          .Size = indirectRegion * m_FramesInFlight,
                                          .Usage = BufferUsage::Storage | BufferUsage::Indirect,
                                      });

        m_CullCountBuffer =
            Buffer::Create(m_Context, {
                                          .Name = "SceneRenderer Cull Count",
                                          .Size = static_cast<u64>(m_FramesInFlight) * sizeof(u32),
                                          .Usage = BufferUsage::Storage | BufferUsage::TransferSrc,
                                          .HostMapped = true,
                                      });

        const AssetResult<AssetHandle<Veng::Shader>> cullCs =
            m_Assets.LoadSync<Veng::Shader>(OcclusionCullCompId);
        VE_ASSERT(cullCs.has_value(), "SceneRenderer: occlusion-cull compute load failed: {}",
                  cullCs.error().Detail);

        m_CullSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Cull Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 1,
                                    .Type = DescriptorType::StorageBuffer,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 2,
                                    .Type = DescriptorType::StorageBuffer,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 3,
                                    .Type = DescriptorType::StorageBuffer,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                               },
                       });
        m_CullLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Cull Layout",
                           .DescriptorSetLayouts = {m_CullSetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<OcclusionCullPush>(
                               ShaderStage::Compute)},
                       });
        m_CullPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Cull Pipeline",
                .PipelineLayout = m_CullLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = cullCs->Get()->Module},
            });

        m_CullSet = DescriptorSet::Create(m_Context, {
                                                         .Name = "SceneRenderer Cull Set",
                                                         .Layout = m_CullSetLayout,
                                                     });
        m_CullSet->Write(0, m_HiZSampleView);
        m_CullSet->Write(1, m_CullCandidateBuffer);
        m_CullSet->Write(2, m_IndirectBuffer);
        m_CullSet->Write(3, m_CullCountBuffer);
    }

    void SceneRenderer::CreateHdr()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_HdrHandle);

        m_HdrImage = Image::Create(m_Context, {
                                                  .Name = "SceneRenderer HDR",
                                                  .Extent = {m_Extent.x, m_Extent.y, 1},
                                                  .Format = HdrFormat,
                                                  .Usage = HdrUsage,
                                              });
        m_HdrView =
            ImageView::Create(m_Context, {.Name = "SceneRenderer HDR View", .Image = m_HdrImage});

        m_HdrHandle = bindless.Register(m_HdrView);
    }

    void SceneRenderer::CreateTaa()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();

        // Recreating (or releasing) invalidates the persisted history, so the next resolve
        // must ignore it until a frame repopulates it.
        m_TaaHistoryReset = true;

        bindless.Release(m_LitHandle);
        bindless.Release(m_TaaHistoryHandle);
        m_LitHandle = {};
        m_TaaHistoryHandle = {};

        if (m_Settings.TAA)
        {
            m_LitImage = Image::Create(m_Context, {
                                                      .Name = "SceneRenderer Lit",
                                                      .Extent = {m_Extent.x, m_Extent.y, 1},
                                                      .Format = HdrFormat,
                                                      .Usage = HdrUsage,
                                                  });
            m_LitView = ImageView::Create(m_Context,
                                          {.Name = "SceneRenderer Lit View", .Image = m_LitImage});
            m_LitHandle = bindless.Register(m_LitView);

            m_TaaHistoryImage = Image::Create(m_Context, {
                                                             .Name = "SceneRenderer TAA History",
                                                             .Extent = {m_Extent.x, m_Extent.y, 1},
                                                             .Format = HdrFormat,
                                                             .Usage = HdrUsage,
                                                         });
            m_TaaHistoryView = ImageView::Create(
                m_Context, {.Name = "SceneRenderer TAA History View", .Image = m_TaaHistoryImage});
            m_TaaHistoryHandle = bindless.Register(m_TaaHistoryView);
        }
        else
        {
            // Drop the resolve targets when TAA is off so the memory is not held for an unused path.
            m_LitImage.reset();
            m_LitView.reset();
            m_TaaHistoryImage.reset();
            m_TaaHistoryView.reset();
        }
    }

    void SceneRenderer::CreateBloom()
    {
        // Bloom operates in linear HDR space before tonemap, sampling bilinearly: the wide
        // COD/tent taps land between texels, so the pyramid's HdrFormat must advertise linear
        // filtering — a capability the point-Load hi-Z reduction never exercises.
        VE_ASSERT(m_Context.IsFormatLinearFilterSupported(HdrFormat),
                  "SceneRenderer: bloom needs SampledImageFilterLinear on the HDR format");

        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_BloomResultHandle);
        bindless.Release(m_BloomMip0Handle);

        // The pyramid stops BloomTileShift levels short of 1×1 so the coarsest level holds a
        // ~8 px edge; the max(1u, …) floor guards a tiny extent (mirroring hi-Z's guard).
        const u32 maxDim = std::max(m_Extent.x, m_Extent.y);
        const u32 mipCount =
            maxDim == 0 ? 1u : std::max(1u, std::bit_width(maxDim) - BloomTileShift);

        m_BloomImage = Image::Create(m_Context, {
                                                    .Name = "SceneRenderer Bloom Pyramid",
                                                    .Extent = {m_Extent.x, m_Extent.y, 1},
                                                    .MipLevels = mipCount,
                                                    .Format = HdrFormat,
                                                    .Usage = BloomPyramidUsage,
                                                });

        // One single-mip storage view per level (the down/up dispatches write each), plus a
        // whole-chain sampled view for the bilinear reads. Storage and sampled access to one
        // mip need distinct views.
        m_BloomMips.clear();
        m_BloomMips.reserve(mipCount);
        for (u32 level = 0; level < mipCount; level++)
        {
            m_BloomMips.push_back(ImageView::Create(
                m_Context, {
                               .Name = fmt::format("SceneRenderer Bloom Mip {} View", level),
                               .Image = m_BloomImage,
                               .BaseMipLevel = level,
                               .MipLevels = 1,
                           }));
        }
        m_BloomSampleView =
            ImageView::Create(m_Context, {
                                             .Name = "SceneRenderer Bloom Sample View",
                                             .Image = m_BloomImage,
                                             .MipLevels = mipCount,
                                         });

        // Clamp-to-edge linear sampler for the bilinear taps; MaxLod covers every level so a
        // per-mip sampled source view's level 0 always resolves.
        m_BloomSampler = Sampler::Create(m_Context, {
                                                        .Name = "SceneRenderer Bloom Sampler",
                                                        .MagFilter = Filter::Linear,
                                                        .MinFilter = Filter::Linear,
                                                        .MipmapMode = MipmapMode::Nearest,
                                                        .AddressModeU = AddressMode::ClampToEdge,
                                                        .AddressModeV = AddressMode::ClampToEdge,
                                                        .AddressModeW = AddressMode::ClampToEdge,
                                                        .AnisotropyEnabled = false,
                                                        .MaxLod = static_cast<f32>(mipCount),
                                                    });

        // The full-resolution composite result the tonemap samples; registered into bindless.
        m_BloomResultImage = Image::Create(m_Context, {
                                                          .Name = "SceneRenderer Bloom Result",
                                                          .Extent = {m_Extent.x, m_Extent.y, 1},
                                                          .Format = HdrFormat,
                                                          .Usage = HdrUsage | ImageUsage::Storage,
                                                      });
        m_BloomResultView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer Bloom Result", .Image = m_BloomResultImage});
        m_BloomResultHandle = bindless.Register(m_BloomResultView);

        // The DebugView::Bloom arm samples pyramid mip 0 (post up-sweep) as a bindless source.
        m_BloomMip0Handle = bindless.Register(m_BloomMips[0]);

        // Per-level descriptor sets. A down step binds the source (HDR for level 0, mip k-1
        // otherwise) + sampler + the destination mip; an up step binds the coarser mip k+1 +
        // sampler + the finer destination mip k. The HDR source is bound per Execute by the
        // down-set-0 write below (it re-registers each Resize/Configure too).
        m_BloomDownSets.clear();
        m_BloomDownSets.reserve(mipCount);
        for (u32 level = 0; level < mipCount; level++)
        {
            Ref<DescriptorSet> set = DescriptorSet::Create(
                m_Context, {
                               .Name = fmt::format("SceneRenderer Bloom Down Set {}", level),
                               .Layout = m_BloomDownUpSetLayout,
                           });
            const Ref<ImageView>& source = level == 0 ? m_HdrView : m_BloomMips[level - 1];
            set->Write(0, source);
            set->Write(1, m_BloomSampler);
            set->Write(2, m_BloomMips[level]);
            m_BloomDownSets.push_back(std::move(set));
        }

        // Up sets indexed by the finer destination level k (0..mipCount-2): read mip k+1,
        // write mip k.
        m_BloomUpSets.clear();
        if (mipCount > 1)
        {
            m_BloomUpSets.reserve(mipCount - 1);
            for (u32 level = 0; level + 1 < mipCount; level++)
            {
                Ref<DescriptorSet> set = DescriptorSet::Create(
                    m_Context, {
                                   .Name = fmt::format("SceneRenderer Bloom Up Set {}", level),
                                   .Layout = m_BloomDownUpSetLayout,
                               });
                set->Write(0, m_BloomMips[level + 1]);
                set->Write(1, m_BloomSampler);
                set->Write(2, m_BloomMips[level]);
                m_BloomUpSets.push_back(std::move(set));
            }
        }

        // Composite: HDR + bloom mip 0 sampled inputs, the linear sampler, and the result dest.
        m_BloomCompositeSet =
            DescriptorSet::Create(m_Context, {
                                                 .Name = "SceneRenderer Bloom Composite Set",
                                                 .Layout = m_BloomCompositeSetLayout,
                                             });
        m_BloomCompositeSet->Write(0, m_HdrView);
        m_BloomCompositeSet->Write(1, m_BloomMips[0]);
        m_BloomCompositeSet->Write(2, m_BloomSampler);
        m_BloomCompositeSet->Write(3, m_BloomResultView);
    }

    void SceneRenderer::Rebuild()
    {
        // Final is the full deferred chain; debug modes terminate after the g-buffer with one blit.
        // Bloom imports are declared only when active.
        //
        // Debug arms force-wire their producing battery pass so the visualized target
        // exists regardless of the corresponding Settings toggle.
        const bool debugBloom = m_Settings.Mode == DebugView::Bloom;
        const bool bloomActive =
            (m_Settings.Mode == DebugView::Final && m_Settings.Bloom) || debugBloom;
        m_BloomActive = bloomActive;

        // TAA is a Final-only resolve: it inserts the resolve + history-copy passes between
        // lighting and the tonemap tail and routes lighting into a separate lit target.
        const bool taaActive = m_Settings.Mode == DebugView::Final && m_Settings.TAA;
        m_TaaActive = taaActive;

        const bool debugShadow = m_Settings.Mode == DebugView::Shadows;
        const bool debugAo = m_Settings.Mode == DebugView::AO;
        // Cascades debug needs the shadow pass wired so cascade constants are written.
        const bool debugCascades = m_Settings.Mode == DebugView::Cascades;
        const bool debugPunctual = m_Settings.Mode == DebugView::PunctualShadows;

        const bool shadowActive = (m_Settings.Mode == DebugView::Final && m_Settings.Shadows) ||
                                  debugShadow || debugCascades;
        m_ShadowActive = shadowActive;
        m_ShadowPass = nullptr;

        const bool punctualShadowActive =
            (m_Settings.Mode == DebugView::Final && m_Settings.PunctualShadows) || debugPunctual;
        m_PunctualShadowActive = punctualShadowActive;
        m_PunctualShadowPass = nullptr;

        const bool ssaoFold = m_Settings.Mode == DebugView::Final && m_Settings.AO;
        const bool ssaoActive = ssaoFold || debugAo;
        m_SsaoActive = ssaoActive;
        m_SsaoPass = nullptr;

        RenderGraph graph(m_Context);
        const ResourceId albedoId = graph.Import("SceneRenderer GBuffer Albedo");
        const ResourceId normalId = graph.Import("SceneRenderer GBuffer Normal");
        const ResourceId ormId = graph.Import("SceneRenderer GBuffer ORM");
        const ResourceId depthId = graph.Import("SceneRenderer GBuffer Depth");
        const ResourceId hdrId = graph.Import("SceneRenderer HDR");
        m_OutputId = graph.Import("SceneRenderer Output");

        m_AlbedoId = albedoId;
        m_NormalId = normalId;
        m_OrmId = ormId;
        m_DepthId = depthId;
        m_HdrId = hdrId;

        // When TAA is active the lighting pass writes a separate lit target the resolve
        // reads; the resolve then writes hdrId, so bloom/tonemap sample the resolved result.
        ResourceId litId{};
        ResourceId taaHistoryId{};
        ResourceId velocityId{};
        if (taaActive)
        {
            litId = graph.Import("SceneRenderer Lit");
            taaHistoryId = graph.Import("SceneRenderer TAA History");
        }
        // The velocity target (G3) is always imported — the g-buffer pass writes it on every
        // frame as a fourth color attachment.
        velocityId = graph.Import("SceneRenderer Velocity");
        m_LitId = litId;
        m_TaaHistoryId = taaHistoryId;
        m_VelocityId = velocityId;
        const ResourceId lightingTargetId = taaActive ? litId : hdrId;

        ResourceId shadowId{};
        if (shadowActive)
        {
            shadowId = graph.Import("SceneRenderer ShadowMap");
        }
        m_ShadowId = shadowId;

        ResourceId punctualShadowId{};
        if (punctualShadowActive)
        {
            punctualShadowId = graph.Import("SceneRenderer PunctualShadowMap");
        }
        m_PunctualShadowId = punctualShadowId;

        if (bloomActive)
        {
            // The pyramid imports one per-mip slot per level (the down/up sweep declares
            // per-level access on these); the result is a single full-resolution import.
            m_BloomChainId = graph.ImportImageMips("SceneRenderer Bloom Pyramid",
                                                   static_cast<u32>(m_BloomMips.size()));
            m_BloomResultId = graph.Import("SceneRenderer Bloom Result");
        }

        TextureHandle ssaoHandle{};
        if (ssaoActive)
        {
            m_SsaoId = graph.Import("SceneRenderer SSAO");
        }

        m_Passes.clear();

        // The shadow pass runs first when active so the graph orders its write before
        // the lighting read.
        Ref<ImageView> shadowAtlasView;
        if (shadowActive)
        {
            auto shadowPass = CreateUnique<ShadowScenePass>(
                m_Context, m_Assets, m_Settings.ShadowResolution, m_Settings.CascadeCount);
            m_ShadowPass = shadowPass.get();
            shadowAtlasView = shadowPass->GetShadowView();
            m_Passes.push_back(std::move(shadowPass));
        }

        // Same ordering reason as the directional pass; the atlas is renderer-owned
        // (set 1 binding 4) and passed through PassIO.
        if (punctualShadowActive)
        {
            auto punctualPass = CreateUnique<PunctualShadowScenePass>(
                m_Context, m_Assets, m_Settings.PunctualShadowResolution);
            m_PunctualShadowPass = punctualPass.get();
            m_Passes.push_back(std::move(punctualPass));
        }

        // Update binding 0 to the wired atlas, or the dummy when shadows are off.
        WriteShadowAtlasBinding(shadowActive ? shadowAtlasView : m_DummyShadowView);

        // The GPU cull arm imports the indirect command buffer so the cull compute pass
        // (StorageBufferWrite) and the geometry pass (IndirectRead) share it through the
        // graph-derived buffer barrier.
        m_IndirectId = ResourceId{};
        if (m_ActiveCull == SceneRendererSettings::CullMode::GPU)
        {
            m_IndirectId = graph.ImportBuffer("SceneRenderer Indirect Commands");
        }

        auto gbufferPass = CreateUnique<GBufferScenePass>(m_Context, m_Extent, &m_Internal->Plan,
                                                          m_ActiveCull, m_IndirectId);
        m_Passes.push_back(std::move(gbufferPass));

        // Created before the tail switch so ssaoHandle is set when the Final arm reads it.
        if (ssaoActive)
        {
            auto ssaoPass =
                CreateUnique<SsaoScenePass>(m_Context, m_SsaoPipeline, m_SamplerHandle, m_Extent);
            m_SsaoPass = ssaoPass.get();
            ssaoHandle = ssaoPass->GetAoHandle();
            m_Passes.push_back(std::move(ssaoPass));
        }

        switch (m_Settings.Mode)
        {
        case DebugView::Final:
        {
            m_Passes.push_back(CreateUnique<DeferredLightingScenePass>(
                m_Context, ssaoFold ? m_SsaoLightingPipeline : m_LightingPipeline, m_Extent,
                ssaoFold, m_ShadowSet, m_ShadowRingStride, m_PunctualRingStride));

            // TAA resolves the lit target into the HDR target the tail samples, so it sits
            // between lighting and the bloom/tonemap tail.
            if (taaActive)
            {
                m_Passes.push_back(CreateUnique<TaaScenePass>(
                    m_Context, m_TaaResolvePipeline, m_TaaCopyPipeline, litId, taaHistoryId, hdrId,
                    depthId, velocityId, m_LitHandle, m_TaaHistoryHandle, m_VelocityHandle,
                    &m_TaaHistoryReset, m_Extent));
            }

            // Tonemap source: bloom composite when bloom is on, raw HDR otherwise.
            ResourceId tonemapSourceId = m_HdrId;
            TextureHandle tonemapSourceHandle = m_HdrHandle;

            if (bloomActive)
            {
                // The bloom down/up/composite compute sweep is declared into the graph by
                // DeclareBloom (after the pass loop); here the tonemap just reads its result.
                tonemapSourceId = m_BloomResultId;
                tonemapSourceHandle = m_BloomResultHandle;
            }

            m_Passes.push_back(
                CreateUnique<PostProcessScenePass>(m_Context, m_TonemapMaterial,
                                                   PostProcessInput{
                                                       .Source = tonemapSourceId,
                                                       .SourceTexture = tonemapSourceHandle,
                                                       .Sampler = m_SamplerHandle,
                                                       .TextureField = "Hdr",
                                                       .SamplerField = "HdrSampler",
                                                   },
                                                   m_OutputId, m_OutputFormat, m_Extent));
            break;
        }
        case DebugView::Albedo:
            m_Passes.push_back(
                CreateUnique<FullscreenBlitScenePass>(m_Context, m_AlbedoBlitPipeline, m_Extent,
                                                      FullscreenBlitScenePass::Source::Albedo));
            break;
        case DebugView::Normal:
            m_Passes.push_back(
                CreateUnique<FullscreenBlitScenePass>(m_Context, m_NormalBlitPipeline, m_Extent,
                                                      FullscreenBlitScenePass::Source::Normal));
            break;
        case DebugView::Depth:
            m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                m_Context, m_DepthBlitPipeline, m_Extent, FullscreenBlitScenePass::Source::Depth));
            break;
        case DebugView::Occlusion:
            m_Passes.push_back(CreateUnique<OrmBlitScenePass>(m_Context, m_OrmBlitPipeline,
                                                              m_Extent, /*channel=*/0));
            break;
        case DebugView::Roughness:
            m_Passes.push_back(CreateUnique<OrmBlitScenePass>(m_Context, m_OrmBlitPipeline,
                                                              m_Extent, /*channel=*/1));
            break;
        case DebugView::Metallic:
            m_Passes.push_back(CreateUnique<OrmBlitScenePass>(m_Context, m_OrmBlitPipeline,
                                                              m_Extent, /*channel=*/2));
            break;
        case DebugView::AO:
            m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                m_Context, m_AoBlitPipeline, m_Extent, FullscreenBlitScenePass::Source::Ao));
            break;
        case DebugView::Shadows:
            // Reads the cascade atlas through the dedicated set (raw depth), not bindless.
            m_Passes.push_back(CreateUnique<ShadowBlitScenePass>(
                m_Context, m_ShadowBlitPipeline, m_Extent, m_ShadowBlitSet,
                ShadowBlitScenePass::Source::Directional));
            break;
        case DebugView::PunctualShadows:
            // Reads the punctual atlas through the dedicated set; binding 0 is
            // rewritten below after the pass set is chosen.
            m_Passes.push_back(CreateUnique<ShadowBlitScenePass>(
                m_Context, m_ShadowBlitPipeline, m_Extent, m_ShadowBlitSet,
                ShadowBlitScenePass::Source::Punctual));
            break;
        case DebugView::Cascades:
            // Tints fragments by cascade selection and writes the output directly (no tonemap tail).
            m_Passes.push_back(CreateUnique<DeferredLightingScenePass>(
                m_Context, m_CascadeDebugPipeline, m_Extent, /*useSsao=*/false, m_ShadowSet,
                m_ShadowRingStride, m_PunctualRingStride, /*writeToOutput=*/true));
            break;
        case DebugView::Bloom:
            // Bloom samples the lit HDR, so the lighting pass runs first; the force-wired bloom
            // sweep (DeclareBloom, after the last pass) writes the pyramid, and the terminal blit
            // shows mip 0 after the up-sweep — the accumulated bloom before composite.
            m_Passes.push_back(CreateUnique<DeferredLightingScenePass>(
                m_Context, ssaoFold ? m_SsaoLightingPipeline : m_LightingPipeline, m_Extent,
                ssaoFold, m_ShadowSet, m_ShadowRingStride, m_PunctualRingStride));
            m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                m_Context, m_AlbedoBlitPipeline, m_Extent, FullscreenBlitScenePass::Source::Bloom));
            break;
        case DebugView::MotionVectors:
            // The g-buffer pass writes the velocity target (G3); this blit colorizes it as an
            // optical-flow field.
            m_Passes.push_back(CreateUnique<FullscreenBlitScenePass>(
                m_Context, m_MotionBlitPipeline, m_Extent,
                FullscreenBlitScenePass::Source::MotionVectors));
            break;
        }

        // Point binding 0 at the punctual atlas for the debug blit (overwrites the
        // cascade/dummy atlas written above).
        if (debugPunctual)
        {
            m_ShadowBlitSet->Write(0, m_PunctualShadowView);
        }

        const PassIO io{
            .GBufferAlbedo = albedoId,
            .GBufferNormal = normalId,
            .GBufferOrm = ormId,
            .GBufferDepth = depthId,
            .AlbedoHandle = m_AlbedoHandle,
            .NormalHandle = m_NormalHandle,
            .OrmHandle = m_OrmHandle,
            .DepthHandle = m_DepthHandle,
            .Hdr = lightingTargetId,
            .HdrHandle = m_HdrHandle,
            .Ssao = m_SsaoId,
            .SsaoHandle = ssaoHandle,
            .BloomMip0 = bloomActive ? m_BloomChainId.Level(0) : ResourceId{},
            .BloomMip0Handle = m_BloomMip0Handle,
            .Velocity = velocityId,
            .VelocityHandle = m_VelocityHandle,
            .SamplerHandle = m_SamplerHandle,
            .ShadowMap = shadowId,
            .ShadowView = shadowAtlasView,
            .PunctualShadowMap = punctualShadowId,
            .PunctualShadowView = m_PunctualShadowView,
            .Output = m_OutputId,
        };

        // Import the hi-Z chain once: the GPU cull samples last frame's pyramid (declared
        // .Sample below for the graph-derived transition into ShaderReadOnly before the cull)
        // and the reduction at the tail writes this frame's pyramid into the same slots.
        m_HiZChainId =
            graph.ImportImageMips("SceneRenderer HiZ", static_cast<u32>(m_HiZMips.size()));

        // The GPU cull compute pass must precede the geometry pass: it writes the
        // indirect commands the geometry pass reads. Declared before the pass loop so it
        // is earlier in the graph's declaration (execution) order than the g-buffer pass.
        if (m_ActiveCull == SceneRendererSettings::CullMode::GPU)
        {
            DeclareCullPass(graph);
        }

        for (const Unique<ScenePass>& pass : m_Passes)
        {
            pass->Configure(m_Settings);
            pass->Resize(m_Extent);

            // The bloom compute sweep reads the lit HDR and writes the result the tonemap
            // samples, so it is declared between the lighting pass and the terminal tonemap
            // pass. The tonemap is the last pass when bloom is active; declare bloom just
            // before it so the graph orders down-sweep(HDR) after lighting and tonemap after
            // the composite.
            if (bloomActive && &pass == &m_Passes.back())
            {
                DeclareBloom(graph);
            }

            pass->Declare(graph, io);
        }

        // The hi-Z reduction runs last so it reduces this frame's completed depth.
        // Nothing samples the pyramid yet — it is built and persisted for the
        // next-frame occlusion test — so it changes no rendered pixel.
        DeclareHiZReduction(graph);

        m_Internal->Graph = graph.Compile();
    }

    void SceneRenderer::DeclareHiZReduction(RenderGraph& graph)
    {
        const u32 mipCount = static_cast<u32>(m_HiZMips.size());

        // One compute dispatch per mip. Dispatch k reads mip k's source and writes mip
        // k; the per-mip graph surface derives the read-after-write barrier between
        // dispatch k's write of mip k and dispatch k+1's read of it. Mip 0's source is
        // the depth target (declared .Sample, reusing the depth import so the barrier
        // chains off the lighting pass's read); a source mip n-1 is declared .StorageRead.
        for (u32 level = 0; level < mipCount; level++)
        {
            // Mip extents (image extent >> level, floored at 1).
            const u32 dstW = std::max(m_Extent.x >> level, 1u);
            const u32 dstH = std::max(m_Extent.y >> level, 1u);
            const u32 srcW = level == 0 ? m_Extent.x : std::max(m_Extent.x >> (level - 1), 1u);
            const u32 srcH = level == 0 ? m_Extent.y : std::max(m_Extent.y >> (level - 1), 1u);

            // The source (depth target or prior mip) binds as a sampled image, so it
            // must be in ShaderReadOnly — declared .Sample, not .StorageRead. The
            // destination is a storage write (General). A prior mip therefore goes
            // General (its write) → ShaderReadOnly (its read as the next source), a
            // graph-derived per-mip transition.
            RenderGraph::PassBuilder builder =
                graph.AddComputePass(fmt::format("HiZ Reduce Mip {}", level));
            if (level == 0)
            {
                builder.Sample(m_DepthId);
            }
            else
            {
                builder.Sample(m_HiZChainId.Level(level - 1));
            }
            builder.StorageWrite(m_HiZChainId.Level(level));

            const Ref<ComputePipeline> pipeline = m_HiZReducePipeline;
            const Ref<DescriptorSet> set = m_HiZReduceSets[level];
            const HiZReducePush push{
                .DestExtent = {dstW, dstH},
                .SourceExtent = {srcW, srcH},
            };
            builder.Execute(
                [pipeline, set, push](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(pipeline);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {set},
                        .FirstSet = 1, // set 0 is reserved for the bindless registry
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(push);
                    cmd.Dispatch((push.DestExtent.x + 7) / 8, (push.DestExtent.y + 7) / 8, 1);
                });
        }
    }

    namespace
    {
        // The dynamic-resolution sub-rect mapping for one mip level of a high-water-mark-allocated
        // chain: the valid extent at that level, and the (scale, clamp) UVs mapping a [0,1] valid
        // UV into the level's valid region. At full resolution ScaleUV is 1 and MaxUV ~1.
        struct MipSubRect
        {
            uvec2 ValidExtent;
            vec2 ScaleUV;
            vec2 MaxUV;
        };

        MipSubRect ComputeMipSubRect(uvec2 validBase, uvec2 allocBase, u32 level)
        {
            const uvec2 valid{std::max(validBase.x >> level, 1u),
                              std::max(validBase.y >> level, 1u)};
            const uvec2 alloc{std::max(allocBase.x >> level, 1u),
                              std::max(allocBase.y >> level, 1u)};
            return {
                .ValidExtent = valid,
                .ScaleUV = vec2(valid) / vec2(alloc),
                .MaxUV = (vec2(valid) - 0.5f) / vec2(alloc),
            };
        }
    }

    void SceneRenderer::DeclareBloom(RenderGraph& graph)
    {
        const u32 mipCount = static_cast<u32>(m_BloomMips.size());

        // Down-sweep: dispatch k samples level k's source (HDR for k=0, mip k-1 otherwise)
        // and writes mip k. Mip 0 fuses the bright-pass + Karis; deeper levels are the plain
        // 13-tap. The per-mip graph surface derives the read-after-write barrier between
        // dispatch k's write and dispatch k+1's read.
        const uvec2 allocExtent = m_Extent;
        for (u32 level = 0; level < mipCount; level++)
        {
            RenderGraph::PassBuilder builder =
                graph.AddComputePass(fmt::format("Bloom Down Mip {}", level));
            if (level == 0)
            {
                builder.Sample(m_HdrId);
            }
            else
            {
                builder.Sample(m_BloomChainId.Level(level - 1));
            }
            builder.StorageWrite(m_BloomChainId.Level(level));

            const Ref<ComputePipeline> pipeline = m_Settings.Kernel == BloomKernel::Kawase
                                                      ? m_BloomDownKawasePipeline
                                                      : m_BloomDownPipeline;
            const Ref<DescriptorSet> set = m_BloomDownSets[level];
            const f32 brightPass = level == 0 ? 1.0f : 0.0f;
            // The source is mip level-1 (the HDR for level 0); its dynamic-resolution sub-rect
            // ratio is at the source's level. Computed at record time from this frame's extent.
            const u32 srcLevel = level == 0 ? 0u : level - 1;
            builder.Execute(
                [pipeline, set, level, srcLevel, allocExtent, brightPass](PassContext& inner)
                {
                    const auto* view = static_cast<const SceneView*>(inner.UserData());
                    VE_ASSERT(view != nullptr, "Bloom down pass: null SceneView");
                    const MipSubRect dst =
                        ComputeMipSubRect(view->RenderExtent, allocExtent, level);
                    const MipSubRect src =
                        ComputeMipSubRect(view->RenderExtent, allocExtent, srcLevel);
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(pipeline);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {set},
                        .FirstSet = 1, // set 0 is reserved for the bindless registry
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(BloomDownPush{
                        .DestExtent = dst.ValidExtent,
                        .SourceScaleUV = src.ScaleUV,
                        .SourceMaxUV = src.MaxUV,
                        .BrightPass = brightPass,
                        .Threshold = view->BloomThreshold,
                    });
                    cmd.Dispatch((dst.ValidExtent.x + 7) / 8, (dst.ValidExtent.y + 7) / 8, 1);
                });
        }

        // Up-sweep: from the coarsest finer level down to mip 0, dispatch k samples the
        // coarser mip k+1 (ShaderReadOnly) and read-modify-writes mip k (General) — two
        // subresources of one image in one pass. The pass declares both a Sample on mip k+1
        // and a StorageWrite on mip k; the StorageWrite orders it after the down-sweep that
        // wrote mip k (a General→General write-after-write barrier the graph derives), and a
        // per-level barrier before the next finer up-step reads mip k.
        for (u32 level = mipCount - 1; level-- > 0;)
        {
            RenderGraph::PassBuilder builder =
                graph.AddComputePass(fmt::format("Bloom Up Mip {}", level));
            builder.Sample(m_BloomChainId.Level(level + 1));
            builder.StorageWrite(m_BloomChainId.Level(level));

            const Ref<ComputePipeline> pipeline = m_Settings.Kernel == BloomKernel::Kawase
                                                      ? m_BloomUpKawasePipeline
                                                      : m_BloomUpPipeline;
            const Ref<DescriptorSet> set = m_BloomUpSets[level];
            const u32 srcLevel = level + 1;
            builder.Execute(
                [pipeline, set, level, srcLevel, allocExtent](PassContext& inner)
                {
                    const auto* view = static_cast<const SceneView*>(inner.UserData());
                    VE_ASSERT(view != nullptr, "Bloom up pass: null SceneView");
                    const MipSubRect dst =
                        ComputeMipSubRect(view->RenderExtent, allocExtent, level);
                    const MipSubRect src =
                        ComputeMipSubRect(view->RenderExtent, allocExtent, srcLevel);
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(pipeline);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {set},
                        .FirstSet = 1,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(BloomUpPush{
                        .DestExtent = dst.ValidExtent,
                        .SourceScaleUV = src.ScaleUV,
                        .SourceMaxUV = src.MaxUV,
                        .Radius = view->BloomRadius,
                    });
                    cmd.Dispatch((dst.ValidExtent.x + 7) / 8, (dst.ValidExtent.y + 7) / 8, 1);
                });
        }

        // Composite over the valid sub-rect: result = hdr + mip0 * Intensity. Samples the HDR
        // target and bloom mip 0 (both at mip-0 sub-rect scale) and stores into the result the
        // terminal tonemap upscales.
        {
            RenderGraph::PassBuilder builder = graph.AddComputePass("Bloom Composite");
            builder.Sample(m_HdrId);
            builder.Sample(m_BloomChainId.Level(0));
            builder.StorageWrite(m_BloomResultId);

            const Ref<ComputePipeline> pipeline = m_BloomCompositePipeline;
            const Ref<DescriptorSet> set = m_BloomCompositeSet;
            builder.Execute(
                [pipeline, set, allocExtent](PassContext& inner)
                {
                    const auto* view = static_cast<const SceneView*>(inner.UserData());
                    VE_ASSERT(view != nullptr, "Bloom composite pass: null SceneView");
                    const MipSubRect r = ComputeMipSubRect(view->RenderExtent, allocExtent, 0);
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(pipeline);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {set},
                        .FirstSet = 1,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(BloomCompositePush{
                        .DestExtent = r.ValidExtent,
                        .SourceScaleUV = r.ScaleUV,
                        .SourceMaxUV = r.MaxUV,
                        .Intensity = view->BloomIntensity,
                    });
                    cmd.Dispatch((r.ValidExtent.x + 7) / 8, (r.ValidExtent.y + 7) / 8, 1);
                });
        }
    }

    void SceneRenderer::ResolveActiveCullMode()
    {
        const bool gpuRequested = m_Settings.Cull == SceneRendererSettings::CullMode::GPU;
        const bool gpuSupported = m_Context.IsGpuDrivenCullingSupported();

        // The GPU path is an optimization, not a correctness requirement: a device lacking
        // multiDrawIndirect / drawIndirectFirstInstance silently runs the CPU path, which
        // renders the same image. The fallback is logged once so it is observable, and
        // GetActiveCullMode() reports the real mode for the debug UI and tests.
        if (gpuRequested && !gpuSupported && !m_GpuCullWarned)
        {
            Log::Warn("SceneRenderer: CullMode::GPU requested but the device lacks "
                      "multiDrawIndirect / drawIndirectFirstInstance; using CullMode::CPU.");
            m_GpuCullWarned = true;
        }

        m_ActiveCull = (gpuRequested && gpuSupported) ? SceneRendererSettings::CullMode::GPU
                                                      : SceneRendererSettings::CullMode::CPU;
    }

    void SceneRenderer::DeclareCullPass(RenderGraph& graph)
    {
        // StorageBufferWrite on the indirect import drives the graph-derived
        // StorageBufferWrite → IndirectRead barrier feeding the geometry pass.
        RenderGraph::PassBuilder builder = graph.AddComputePass("Scene GPU Cull");
        builder.StorageBufferWrite(m_IndirectId);

        // The cull samples last frame's hi-Z pyramid (through m_CullSet, off the graph's
        // descriptor binding). Declaring .Sample on each mip drives the graph-derived
        // transition into ShaderReadOnly before the cull — the pyramid is a cross-frame
        // renderer-owned resource the graph would otherwise leave in its prior layout.
        for (u32 level = 0; level < m_HiZMips.size(); ++level)
        {
            builder.Sample(m_HiZChainId.Level(level));
        }

        builder.Execute(
            [this](PassContext& inner)
            {
                const GBufferDrawPlan& plan = m_Internal->Plan;
                if (plan.Cull != SceneRendererSettings::CullMode::GPU || plan.Slots.empty())
                {
                    return;
                }

                CommandBuffer& cmd = inner.Cmd();
                cmd.BindPipeline(m_CullPipeline);
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {m_CullSet},
                    .FirstSet = 1, // set 0 is reserved for the bindless registry
                    .PipelineBindPoint = PipelineBindPoint::Compute,
                });
                cmd.PushConstants(OcclusionCullPush{
                    .PrevViewProj = m_CullPrevViewProj,
                    .HiZBaseExtent = m_CullHiZExtent,
                    .CandidateCount = m_CullCandidateCount,
                    .HistoryValid = m_CullHistoryValid,
                    // Small bias absorbing reduction quantization; a tie draws.
                    .DepthBias = 0.001f,
                    .FrameBase = m_CullFrameBase,
                    .CountIndex = m_CullCountIndex,
                });
                cmd.Dispatch((m_CullCandidateCount + 63) / 64, 1, 1);
            });
    }

    void SceneRenderer::PrepareDraws(const SceneView& view, const u32 viewConstantsIndex)
    {
        GBufferDrawPlan& plan = m_Internal->Plan;
        plan.Cull = m_ActiveCull;
        plan.DrawDataSet = m_DrawDataSet;
        plan.CandidateIdBuffer = m_CandidateIdBuffer;
        plan.IndirectBuffer = m_IndirectBuffer;
        plan.PipelineMaterial = nullptr;
        plan.Slots.clear();
        plan.Groups.clear();
        plan.SkinnedPipelineMaterial = nullptr;
        plan.PaletteSet = m_PaletteSet;
        plan.SkinnedSlots.clear();
        plan.SkinnedGroups.clear();
        m_PaletteBaseByEntity.clear();

        const u32 frameIndex = m_Context.GetBindlessRegistry().GetCurrentViewConstantsIndex();
        const u32 frameBase = frameIndex * MaxCullCandidates;
        const u32 paletteRegionBase = frameIndex * MaxSkinningMatricesPerFrame;
        u32 paletteCursor = 0;
        auto* paletteData = static_cast<mat4*>(m_PaletteBuffer->GetMappedData());
        plan.Push = SurfacePush{.FrameBase = frameBase, .ViewConstantsIndex = viewConstantsIndex};
        plan.IndirectRegionOffset =
            frameIndex * MaxCullCandidates * static_cast<u32>(sizeof(DrawIndexedIndirectCommand));

        const std::span<const SubMeshCandidate> candidates =
            view.Broadphase->GetSubMeshCandidates();

        // The camera-frustum survivors, in ascending Cull-id order; the upload source for
        // both modes (the GPU compute adds only occlusion, never re-running the frustum).
        m_CullScratch.clear();
        if (m_Settings.FrustumCull)
        {
            const Frustum cameraFrustum = Frustum::FromViewProjection(view.Camera.ViewProjection());
            view.Broadphase->Cull(cameraFrustum, m_CullScratch);
        }
        else
        {
            m_CullScratch.reserve(candidates.size());
            for (u32 i = 0; i < candidates.size(); ++i)
            {
                m_CullScratch.push_back(i);
            }
        }

        // The per-submesh frustum survivors are both the frustum-survived and the drawn funnel
        // stage: a survivor is a draw under CullMode::CPU (a materialless or not-yet-resident one
        // counts even though it fills no slot below — the per-submesh cull result the draw-count
        // fixtures assert on). The occlusion stage shows up separately as GetLastGpuSurvivorCount.
        m_FrustumSurvivedCount = static_cast<u32>(m_CullScratch.size());
        m_LastDrawnCount = m_FrustumSurvivedCount;

        // Per-object velocity needs each drawn entity's previous-frame world matrix. Record
        // this frame's worlds (swapped to "previous" after Execute) and look the prior one up
        // when filling DrawData. The surface pass writes velocity every frame (G3), so this is
        // always maintained.
        const auto PackEntity = [](Entity e) -> u64
        { return (static_cast<u64>(e.Index) << 32) | static_cast<u64>(e.Generation); };
        {
            m_CurrentWorlds.clear();
            for (const VisibleMesh& vm : view.Visible)
            {
                m_CurrentWorlds[PackEntity(vm.Owner)] = vm.World;
            }
        }

        auto* drawData = static_cast<GpuDrawData*>(m_DrawDataBuffer->GetMappedData());
        GpuCullCandidate* cullData =
            m_CullCandidateBuffer
                ? static_cast<GpuCullCandidate*>(m_CullCandidateBuffer->GetMappedData()) +
                      static_cast<usize>(frameBase)
                : nullptr;

        // Skinned survivors are deferred to a second pass (they draw on the CPU-direct skinned
        // path after the static slots, which must stay contiguous from 0 for the GPU cull arrays).
        vector<u32> skinnedScratch;

        // Fill one slot per survivor whose submesh has a loaded material (a materialless or
        // not-yet-resident submesh is skipped, matching the direct draw it replaces). The slot
        // index is the dense candidate id the instance attribute carries.
        for (const u32 id : m_CullScratch)
        {
            const SubMeshCandidate& candidate = candidates[id];
            const VisibleMesh& item = view.Visible[candidate.MeshCandidate];
            const Mesh& mesh = *item.Mesh;
            const std::span<const AssetHandle<Material>> materials = mesh.GetMaterials();
            const SubMesh& subMesh = mesh.GetSubMeshes()[candidate.SubMeshIndex];

            if (subMesh.MaterialIndex == SubMesh::NoMaterial ||
                !materials[subMesh.MaterialIndex].IsLoaded())
            {
                continue;
            }

            if (mesh.IsSkinned())
            {
                skinnedScratch.push_back(id);
                continue;
            }

            const u32 slot = static_cast<u32>(plan.Slots.size());
            if (slot >= MaxCullCandidates)
            {
                VE_ASSERT(false,
                          "SceneRenderer: per-frame candidate count exceeds MaxCullCandidates {}",
                          MaxCullCandidates);
                break;
            }

            const Material& material = *materials[subMesh.MaterialIndex].Get();
            if (!plan.PipelineMaterial)
            {
                plan.PipelineMaterial = materials[subMesh.MaterialIndex].Get();
            }

            // Per-draw record: world matrix, the normal matrix's three columns (inverse-
            // transpose of the upper 3×3, correct under non-uniform scale), and the
            // frame-folded material selector.
            const mat3 normalMatrix = glm::inverseTranspose(mat3(item.World));
            // Previous world for velocity: last frame's matrix for this entity, or the
            // current one (zero object motion) when first seen.
            mat4 prevWorld = item.World;
            {
                const auto it = m_PreviousWorlds.find(PackEntity(item.Owner));
                if (it != m_PreviousWorlds.end())
                {
                    prevWorld = it->second;
                }
            }
            drawData[frameBase + slot] = GpuDrawData{
                .World = item.World,
                .NormalColumn0 = vec4(normalMatrix[0], 0.0f),
                .NormalColumn1 = vec4(normalMatrix[1], 0.0f),
                .NormalColumn2 = vec4(normalMatrix[2], 0.0f),
                .MaterialIndex = material.GetMaterialSelector(),
                .PrevWorld = prevWorld,
            };

            if (cullData != nullptr)
            {
                const AABB& bounds = item.WorldBounds;
                cullData[slot] = GpuCullCandidate{
                    .BoundsMin = vec4(bounds.Min, 0.0f),
                    .BoundsMax = vec4(bounds.Max, 0.0f),
                    .IndexCount = subMesh.IndexCount,
                    .FirstIndex = subMesh.IndexOffset,
                    .VertexOffset = 0,
                    .FirstInstance = slot,
                };
            }

            plan.Slots.push_back(DrawSlot{
                .SourceMesh = &mesh,
                .IndexCount = subMesh.IndexCount,
                .FirstIndex = subMesh.IndexOffset,
                .VertexOffset = 0,
                .CandidateId = slot,
            });
        }

        // Group contiguous slots that share a source mesh so the mesh's buffers bind once.
        for (u32 s = 0; s < plan.Slots.size();)
        {
            const Mesh* mesh = plan.Slots[s].SourceMesh;
            u32 count = 0;
            while (s + count < plan.Slots.size() && plan.Slots[s + count].SourceMesh == mesh)
            {
                ++count;
            }
            plan.Groups.push_back(
                DrawGroup{.SourceMesh = mesh, .FirstSlot = s, .SlotCount = count});
            s += count;
        }

        // Skinned second pass: assign DrawData slots after the static ones (keeping the static
        // range contiguous from 0), compute each skinned instance's palette once per entity, and
        // record PaletteBase into the slot's DrawData. These draw on the CPU-direct skinned path.
        for (const u32 id : skinnedScratch)
        {
            const SubMeshCandidate& candidate = candidates[id];
            const VisibleMesh& item = view.Visible[candidate.MeshCandidate];
            const Mesh& mesh = *item.Mesh;
            const SubMesh& subMesh = mesh.GetSubMeshes()[candidate.SubMeshIndex];
            const std::span<const AssetHandle<Material>> materials = mesh.GetMaterials();
            const AssetHandle<Skeleton>& skeletonHandle = mesh.GetSkeleton();
            if (!skeletonHandle.IsLoaded())
            {
                continue;
            }
            const Skeleton& skeleton = *skeletonHandle.Get();
            const u32 boneCount = static_cast<u32>(skeleton.GetBoneCount());

            // One palette per entity, shared by its submeshes. Computed on first encounter from
            // the entity's SkinnedPose (the animation system's output) or the bind pose when the
            // entity has none (e.g. the editor with systems paused).
            const u64 packed = PackEntity(item.Owner);
            u32 paletteBase = 0;
            const auto existing = m_PaletteBaseByEntity.find(packed);
            if (existing != m_PaletteBaseByEntity.end())
            {
                paletteBase = existing->second;
            }
            else
            {
                if (paletteCursor + boneCount > MaxSkinningMatricesPerFrame)
                {
                    continue; // palette budget exhausted this frame
                }
                paletteBase = paletteRegionBase + paletteCursor;

                const auto* pose = view.World.TryGet<SkinnedPose>(item.Owner);
                if (pose != nullptr && pose->Skinning.size() == boneCount)
                {
                    std::memcpy(paletteData + paletteBase, pose->Skinning.data(),
                                static_cast<usize>(boneCount) * sizeof(mat4));
                }
                else
                {
                    vector<mat4> bind;
                    skeleton.ComputeBindPoseMatrices(bind);
                    std::memcpy(paletteData + paletteBase, bind.data(),
                                static_cast<usize>(boneCount) * sizeof(mat4));
                }

                paletteCursor += boneCount;
                m_PaletteBaseByEntity[packed] = paletteBase;
            }

            const u32 slot = static_cast<u32>(plan.Slots.size() + plan.SkinnedSlots.size());
            if (slot >= MaxCullCandidates)
            {
                break;
            }

            const Material& material = *materials[subMesh.MaterialIndex].Get();
            if (plan.SkinnedPipelineMaterial == nullptr)
            {
                plan.SkinnedPipelineMaterial = materials[subMesh.MaterialIndex].Get();
            }

            // Velocity needs the previous frame's world and palette base for this entity (its
            // deformation motion). The previous palette data is still resident in its own ring
            // region. First seen → no motion (current values).
            mat4 prevWorld = item.World;
            u32 prevPaletteBase = paletteBase;
            {
                const auto prevWorldIt = m_PreviousWorlds.find(packed);
                if (prevWorldIt != m_PreviousWorlds.end())
                {
                    prevWorld = prevWorldIt->second;
                }
                const auto prevBaseIt = m_PreviousPaletteBaseByEntity.find(packed);
                if (prevBaseIt != m_PreviousPaletteBaseByEntity.end())
                {
                    prevPaletteBase = prevBaseIt->second;
                }
            }

            const mat3 normalMatrix = glm::inverseTranspose(mat3(item.World));
            drawData[frameBase + slot] = GpuDrawData{
                .World = item.World,
                .NormalColumn0 = vec4(normalMatrix[0], 0.0f),
                .NormalColumn1 = vec4(normalMatrix[1], 0.0f),
                .NormalColumn2 = vec4(normalMatrix[2], 0.0f),
                .MaterialIndex = material.GetMaterialSelector(),
                .PaletteBase = paletteBase,
                .PrevPaletteBase = prevPaletteBase,
                .PrevWorld = prevWorld,
            };

            plan.SkinnedSlots.push_back(DrawSlot{
                .SourceMesh = &mesh,
                .IndexCount = subMesh.IndexCount,
                .FirstIndex = subMesh.IndexOffset,
                .VertexOffset = 0,
                .CandidateId = slot,
            });
        }

        for (u32 s = 0; s < plan.SkinnedSlots.size();)
        {
            const Mesh* mesh = plan.SkinnedSlots[s].SourceMesh;
            u32 count = 0;
            while (s + count < plan.SkinnedSlots.size() &&
                   plan.SkinnedSlots[s + count].SourceMesh == mesh)
            {
                ++count;
            }
            plan.SkinnedGroups.push_back(
                DrawGroup{.SourceMesh = mesh, .FirstSlot = s, .SlotCount = count});
            s += count;
        }

        // The GPU cull dispatch reads the candidate region this frame; zero its survivor
        // count so the next-frame readback reflects only this dispatch. The push members the
        // cull pass reads are set here (per-frame), not in the recompile-time declaration.
        if (m_ActiveCull == SceneRendererSettings::CullMode::GPU)
        {
            const u32 count = static_cast<u32>(plan.Slots.size());
            m_CullCandidateCount = count;
            m_CullFrameBase = frameBase;
            m_CullCountIndex = frameIndex;
            m_CullPrevViewProj = m_PreviousViewProj;
            m_CullHiZExtent = m_Extent;
            m_CullHistoryValid = (m_Settings.Occlusion && m_HiZHistoryValid) ? 1u : 0u;

            auto* counts = static_cast<u32*>(m_CullCountBuffer->GetMappedData());
            counts[frameIndex] = 0;

            // Record the region the readback reads one frame late.
            m_GpuCandidateCount = count;
            m_GpuReadbackRegion = frameIndex;
            m_GpuReadbackValid = true;
        }
    }

    void SceneRenderer::Resize(const uvec2 extent)
    {
        m_Extent = extent;
        // The new allocation is the full target until the next Execute scales it; the prior
        // sub-rect mapping is moot (the TAA history resets on resize anyway).
        m_ValidExtent = extent;
        m_PreviousRenderScaleUV = vec2(1.0f);
        m_PreviousMaxValidUV = vec2(1.0f);
        CreateOutput();
        CreateGBuffer();
        CreateHdr();
        CreateTaa();
        CreateBloom();
        CreatePunctualShadowAtlas();
        Rebuild();
    }

    u32 SceneRenderer::GetMaxShadowResolution() const
    {
        // The directional atlas is widest at the largest cascade grid (2×2 at four
        // cascades), so a tile larger than the device limit / 2 would overflow it.
        const ShadowAtlasGrid grid = ComputeShadowAtlasGrid(MaxCascades);
        const u32 factor = std::max(grid.Columns, grid.Rows);
        return m_Context.GetMaxImageDimension2D() / factor;
    }

    u32 SceneRenderer::GetMaxPunctualShadowResolution() const
    {
        // The punctual atlas tiles CubeFaceCount columns × MaxShadowedPunctual rows,
        // so its widest side is CubeFaceCount · resolution.
        const u32 factor = std::max(CubeFaceCount, MaxShadowedPunctual);
        return m_Context.GetMaxImageDimension2D() / factor;
    }

    void SceneRenderer::ClampShadowResolutions()
    {
        m_Settings.ShadowResolution =
            std::min(m_Settings.ShadowResolution, GetMaxShadowResolution());
        m_Settings.PunctualShadowResolution =
            std::min(m_Settings.PunctualShadowResolution, GetMaxPunctualShadowResolution());
    }

    void SceneRenderer::Configure(const SceneRendererSettings& settings)
    {
        m_Settings = settings;
        ClampShadowResolutions();
        ResolveActiveCullMode();
        CreateTaa();
        CreatePunctualShadowAtlas();
        Rebuild();
    }

    void SceneRenderer::Execute(CommandBuffer& cmd, const SceneView& view)
    {
        // Dynamic resolution: render into the top-left round(allocExtent * scale) sub-rect of the
        // (high-water-mark-allocated) targets; the terminal tonemap upscales it to the full output.
        // Scale applies only on the Final path with the sub-rect-aware battery set: a debug view, the
        // TAA resolve, the GPU hi-Z occlusion test, and the Dual-Kawase bloom kernel do not carry the
        // sub-rect sampling yet, so each forces full resolution (correct, just no scaling).
        const bool drsSupported =
            m_Settings.Mode == DebugView::Final && !m_Settings.TAA &&
            !(m_ActiveCull == SceneRendererSettings::CullMode::GPU && m_Settings.Occlusion) &&
            !(m_Settings.Bloom && m_Settings.Kernel == BloomKernel::Kawase);
        const f32 renderScale = drsSupported ? view.RenderScale : 1.0f;
        const uvec2 validExtent =
            glm::clamp(uvec2(glm::round(vec2(m_Extent) * renderScale)), uvec2(1), m_Extent);
        const vec2 renderScaleUV = vec2(validExtent) / vec2(m_Extent);
        // Half-texel inset so a bilinear tap at the valid edge never reads past it.
        const vec2 maxValidUV = (vec2(validExtent) - 0.5f) / vec2(m_Extent);

        // Per-frame param writes land in the ring-buffered block's current region (no stall).
        if (m_TonemapMaterial.IsLoaded())
        {
            auto& tonemap = const_cast<Material&>(*m_TonemapMaterial.Get());
            tonemap.SetParam("Exposure", view.Exposure);
            // The terminal tonemap reads the sub-rect HDR and upscales it to the full output.
            tonemap.SetParam("RenderScale", vec4(renderScaleUV, maxValidUV));
        }

        // Pack every Light entity into the GPU light layout: directional selection,
        // punctual shadow-slot assignment, and the std430 per-light records. The first
        // MaxShadowedPunctual point/spot lights are assigned a shadow slot (Cone.z carries
        // it, -1 = unshadowed); their records ride set-1 binding 3.
        const PackedSceneLights packed = PackSceneLights(view.World, m_Settings.PunctualShadows,
                                                         m_Settings.PunctualShadowResolution);

        // Mirror filled records into the GPU block (unused slots stay zeroed → type 0 = "no map").
        // AtlasParams.x = 1/tileRes, used by the lighting pass for inset clamping and PCF.
        PunctualShadowBlock punctualBlock{};
        for (u32 s = 0; s < packed.PunctualCount; ++s)
        {
            punctualBlock.Records[s] = packed.PunctualRecords[s];
        }
        punctualBlock.AtlasParams =
            vec4(1.0f / static_cast<f32>(m_Settings.PunctualShadowResolution), 0.0f, 0.0f, 0.0f);

        // Sync the broadphase: re-gathers and rebuilds only when the scene's spatial
        // version moved or a mesh finished loading. The scene passes then query its tree.
        // sceneBounds is the bound union from the same gather, so no separate SceneBounds call is needed.
        m_Broadphase.Sync(view.World);
        const AABB sceneBounds = m_Broadphase.GetSceneBounds();

        // sceneBounds extends only the per-cascade light-axis near plane (off-screen casters);
        // the cascade XY extent comes from the camera frustum slice.
        const CascadeData cascades =
            ComputeCascades(view.Camera, packed.DirectionalTravel, sceneBounds,
                            {.Count = m_Settings.CascadeCount,
                             .Lambda = m_Settings.CascadeSplitLambda,
                             .Resolution = m_Settings.ShadowResolution});

        // Thread the raw (non-tile-remapped) cascade matrices to the shadow pass,
        // which renders each cascade with its viewport placing it in the atlas tile.
        SceneView resolvedView = view;
        resolvedView.RenderExtent = validExtent;
        resolvedView.LightCount = packed.LightCount;
        resolvedView.CascadeViewProj = cascades.ViewProj;
        resolvedView.CascadeCount = cascades.Count;
        resolvedView.PunctualShadows = packed.PunctualRecords;
        resolvedView.PunctualShadowCount = packed.PunctualCount;
        resolvedView.PunctualShadowRawViewProj = packed.PunctualRawViewProj;
        resolvedView.Visible = m_Broadphase.GetCandidates();
        resolvedView.Broadphase = &m_Broadphase;

        BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        registry.WriteLights(std::as_bytes(std::span(packed.Lights.data(), packed.LightCount)));

        // Pack view constants (camera/view state only; shadow system rides set-1).
        // The unjittered view-projection drives the frustum cull, hi-Z, and next frame's
        // reprojection matrix; the jittered one (TAA only) is what the geometry and
        // lighting actually render through.
        const mat4 viewProj = view.Camera.ViewProjection();
        mat4 renderProj = view.Camera.Projection();
        if (m_TaaActive && validExtent.x > 0 && validExtent.y > 0)
        {
            // Sub-pixel projection shear; sign is irrelevant to quality (the sequence is
            // symmetric) and cancels between render and reconstruction, which share this
            // matrix. The reprojection uses the separate unjittered PrevViewProj. The jitter is
            // a fraction of the rendered (sub-rect) extent, the resolution actually rasterized.
            const vec2 jitterPixel = TaaJitterOffset(m_FrameIndex);
            renderProj[2][0] += 2.0f * jitterPixel.x / static_cast<f32>(validExtent.x);
            renderProj[2][1] += 2.0f * jitterPixel.y / static_cast<f32>(validExtent.y);
        }
        const mat4 renderViewProj = renderProj * view.Camera.View();
        const ViewConstantsBlock viewConstants{
            .InvViewProj = glm::inverse(renderViewProj),
            .CameraPosition = vec4(view.Camera.GetPosition(), 0.0f),
            .View = view.Camera.View(),
            .Proj = renderProj,
            .PrevViewProj = m_PreviousViewProj,
            .CurViewProj = viewProj,
            .RenderScaleUV = vec4(renderScaleUV, m_PreviousRenderScaleUV),
            .MaxValidUV = vec4(maxValidUV, m_PreviousMaxValidUV),
        };
        registry.WriteViewConstants(std::as_bytes(std::span(&viewConstants, 1)));

        // Decide whether last frame's pyramid is trustworthy this frame. The reset
        // flag (frame 0 / post-resize / post-configure) forces invalid regardless of
        // the view delta; otherwise the device-free metric compares this frame's
        // camera against last frame's. The result feeds the GPU cull (occlusion skipped
        // when invalid); this plan lands it for tests and the cull to consume.
        const mat4 invView = glm::inverse(view.Camera.View());
        const HiZHistoryState currentHiZState{
            .CameraPosition = view.Camera.GetPosition(),
            // The camera looks down -Z in view space; its world forward is the negated
            // third basis column of the view's inverse.
            .CameraForward = glm::normalize(-vec3(invView[2])),
            .Projection = view.Camera.Projection(),
        };
        const f32 sceneDiagonal = sceneBounds.IsEmpty() ? 0.0f : glm::length(sceneBounds.Size());
        m_HiZHistoryValid =
            !m_HiZHistoryReset && Renderer::IsHiZHistoryValid(m_PreviousHiZState, currentHiZState,
                                                              sceneDiagonal, HiZHistorySettings{});

        // Pack set-1 ShadowConstants: tile-remapped cascade view-projs, splits, and
        // params. Enabled only when the shadow pass is wired AND a directional light
        // exists this frame; otherwise the lighting pass reads full visibility.
        const bool shadowEnabled = m_ShadowActive && m_ShadowPass && packed.HaveDirectional;
        const ShadowAtlasGrid grid = ComputeShadowAtlasGrid(m_Settings.CascadeCount);

        ShadowConstantsBlock shadowConstants{};
        for (u32 k = 0; k < cascades.Count && k < MaxCascades; ++k)
        {
            shadowConstants.CascadeViewProj[k] =
                ComposeTileRemap(cascades.ViewProj[k], k, grid.Columns, grid.Rows);
            shadowConstants.CascadeSplits[k] = cascades.SplitFar[k];
        }
        // Blend band: a view-space distance before each cascade's far split where the
        // lighting pass cross-fades into the next cascade. Sized from the first (smallest)
        // cascade so the band never exceeds any cascade's own range.
        const f32 firstSplit = cascades.Count > 0 ? cascades.SplitFar[0] : 0.0f;
        const f32 blendBand = firstSplit * 0.1f;

        shadowConstants.ShadowParams =
            vec4(1.0f / static_cast<f32>(m_Settings.ShadowResolution), blendBand,
                 static_cast<f32>(cascades.Count), shadowEnabled ? 1.0f : 0.0f);

        // Write only the current frame's region (not yet submitted; safe).
        // The bind selects it via dynamic offset frame * stride.
        const u32 frameIndex = registry.GetCurrentViewConstantsIndex();
        std::memcpy(static_cast<u8*>(m_ShadowConstantsBuffer->GetMappedData()) +
                        static_cast<usize>(frameIndex) * m_ShadowRingStride,
                    &shadowConstants, sizeof(ShadowConstantsBlock));

        // Flush punctual records into this frame's binding-3 region (same safe write).
        // Unused slots stay zeroed → type 0 = "no map".
        std::memcpy(static_cast<u8*>(m_PunctualShadowBuffer->GetMappedData()) +
                        static_cast<usize>(frameIndex) * m_PunctualRingStride,
                    &punctualBlock, sizeof(PunctualShadowBlock));

        // Read the GPU survivor count the previous Execute wrote into this frame's region
        // before PrepareDraws zeroes it again — the host-visible count is one frame late, so
        // it never gates this frame's draw. Only meaningful under the GPU path.
        if (m_ActiveCull == SceneRendererSettings::CullMode::GPU && m_CullCountBuffer)
        {
            const auto* counts = static_cast<const u32*>(m_CullCountBuffer->GetMappedData());
            m_LastGpuSurvivorCount = counts[frameIndex];
        }

        // Fill the per-draw DrawData buffer + (GPU) the candidate buffer and submission plan.
        PrepareDraws(resolvedView, frameIndex);

        // Expose the skinning palette + per-entity bases (filled by PrepareDraws) to the shadow
        // passes so a skinned caster casts its posed shadow.
        resolvedView.SkinningPalette = m_PaletteSet;
        resolvedView.SkinnedPaletteBases = &m_PaletteBaseByEntity;

        // Bloom and SSAO imports are appended only when active (they are only declared then).
        vector<RenderGraph::ImportBinding> bindings = {
            {m_AlbedoId, m_AlbedoView}, {m_NormalId, m_NormalView}, {m_OrmId, m_OrmView},
            {m_DepthId, m_DepthView},   {m_HdrId, m_HdrView},       {m_OutputId, m_OutputView},
        };
        if (m_TaaActive)
        {
            bindings.push_back({m_LitId, m_LitView});
            bindings.push_back({m_TaaHistoryId, m_TaaHistoryView});
        }
        // Velocity is a g-buffer channel the surface pass writes every frame, so it is always bound.
        bindings.push_back({m_VelocityId, m_VelocityView});
        if (m_ShadowActive && m_ShadowPass)
        {
            bindings.push_back({m_ShadowId, m_ShadowPass->GetShadowView()});
        }
        if (m_PunctualShadowActive && m_PunctualShadowPass)
        {
            bindings.push_back({m_PunctualShadowId, m_PunctualShadowView});
        }
        if (m_BloomActive)
        {
            // Each pyramid mip binds its per-frame storage view to its per-mip import slot
            // (the down/up sweep declared per-level access on these); the result is one slot.
            for (u32 level = 0; level < m_BloomMips.size(); level++)
            {
                bindings.push_back({m_BloomChainId.Level(level), m_BloomMips[level]});
            }
            bindings.push_back({m_BloomResultId, m_BloomResultView});
        }
        if (m_SsaoActive && m_SsaoPass != nullptr)
        {
            bindings.push_back({m_SsaoId, m_SsaoPass->GetAoView()});
        }
        // Bind each hi-Z mip's per-frame storage view to its per-mip import slot.
        for (u32 level = 0; level < m_HiZMips.size(); level++)
        {
            bindings.push_back({m_HiZChainId.Level(level), m_HiZMips[level]});
        }
        // The GPU cull arm shares the indirect command buffer between the cull pass and the
        // geometry pass through this import (the same buffer the cull set binding 2 writes).
        if (m_ActiveCull == SceneRendererSettings::CullMode::GPU)
        {
            bindings.push_back({.Id = m_IndirectId, .Buffer = m_IndirectBuffer});
        }
        m_Internal->Graph->Execute(cmd, bindings, &resolvedView);

        // Capture this frame's camera + matrix for next frame's history comparison and
        // occlusion test: the reduction declared in this graph wrote the pyramid from
        // this frame's depth, so it pairs with this frame's view-projection next time.
        m_PreviousViewProj = viewProj;
        m_PreviousHiZState = currentHiZState;

        // Record this frame's sub-rect mapping: GetValidExtent reports it, and the next frame's
        // TAA resolve reprojects the history written at this scale (the zw of RenderScaleUV).
        m_ValidExtent = validExtent;
        m_PreviousRenderScaleUV = renderScaleUV;
        m_PreviousMaxValidUV = maxValidUV;
        // The pyramid now holds this frame's depth, so the next Execute may test against
        // it (subject to the view-delta metric).
        m_HiZHistoryReset = false;

        // The history-copy pass populated the history this frame, so the next resolve may
        // reproject against it. Advance the jitter sequence regardless of the TAA toggle so
        // enabling it mid-run does not restart the phase.
        m_TaaHistoryReset = false;
        ++m_FrameIndex;

        // This frame's worlds + skinning-palette bases become next frame's "previous" for the
        // velocity channel. The palette buffer is ring-buffered, so the previous bases still point
        // at resident data next frame.
        m_PreviousWorlds.swap(m_CurrentWorlds);
        m_PreviousPaletteBaseByEntity = m_PaletteBaseByEntity;
    }

    Ref<ImageView> SceneRenderer::GetOutput() const
    {
        return m_OutputView;
    }

    uvec2 SceneRenderer::GetValidExtent() const
    {
        return m_ValidExtent;
    }

    u32 SceneRenderer::GetLastVisibleCount() const
    {
        return static_cast<u32>(m_Broadphase.GetSubMeshCandidates().size());
    }
    SceneRendererSettings::CullMode SceneRenderer::GetActiveCullMode() const
    {
        return m_ActiveCull;
    }
    u32 SceneRenderer::GetLastGpuSurvivorCount() const
    {
        return m_LastGpuSurvivorCount;
    }
    vector<u32> SceneRenderer::ReadbackGpuSurvivorFlags() const
    {
        if (!m_GpuReadbackValid || m_GpuCandidateCount == 0 || !m_IndirectBuffer)
        {
            return {};
        }

        // Download the full indirect buffer and pull each candidate command's instanceCount
        // from this frame's region; 1 = drawn, 0 = occluded.
        const vector<u8> bytes = m_IndirectBuffer->Download();
        const auto* commands = reinterpret_cast<const DrawIndexedIndirectCommand*>(bytes.data());
        const u32 base = m_GpuReadbackRegion * MaxCullCandidates;

        vector<u32> flags(m_GpuCandidateCount);
        for (u32 i = 0; i < m_GpuCandidateCount; ++i)
        {
            flags[i] = commands[base + i].InstanceCount;
        }
        return flags;
    }
    u32 SceneRenderer::GetFrustumSurvivedCount() const
    {
        return m_FrustumSurvivedCount;
    }
    u32 SceneRenderer::GetLastDrawnCount() const
    {
        return m_LastDrawnCount;
    }
    bool SceneRenderer::DidBroadphaseRebuildLastFrame() const
    {
        return m_Broadphase.DidRebuildLastSync();
    }
    u32 SceneRenderer::GetBroadphaseNodeCount() const
    {
        return m_Broadphase.GetNodeCount();
    }
    Ref<ImageView> SceneRenderer::GetAlbedoView() const
    {
        return m_AlbedoView;
    }
    Ref<ImageView> SceneRenderer::GetNormalView() const
    {
        return m_NormalView;
    }
    Ref<ImageView> SceneRenderer::GetOrmView() const
    {
        return m_OrmView;
    }
    Ref<ImageView> SceneRenderer::GetDepthView() const
    {
        return m_DepthView;
    }
    Ref<ImageView> SceneRenderer::GetHiZView() const
    {
        return m_HiZSampleView;
    }
    Ref<ImageView> SceneRenderer::GetHiZMipView(const u32 level) const
    {
        VE_ASSERT(level < m_HiZMips.size(), "SceneRenderer::GetHiZMipView: level {} out of range",
                  level);
        return m_HiZMips[level];
    }
    u32 SceneRenderer::GetHiZMipCount() const
    {
        return static_cast<u32>(m_HiZMips.size());
    }
    bool SceneRenderer::IsHiZHistoryValid() const
    {
        return m_HiZHistoryValid;
    }
    mat4 SceneRenderer::GetPreviousViewProj() const
    {
        return m_PreviousViewProj;
    }
    Ref<ImageView> SceneRenderer::GetHdrView() const
    {
        return m_HdrView;
    }
    Ref<ImageView> SceneRenderer::GetBloomResultView() const
    {
        return m_BloomResultView;
    }
    Ref<ImageView> SceneRenderer::GetTaaHistoryView() const
    {
        return m_TaaHistoryView;
    }
    Ref<ImageView> SceneRenderer::GetVelocityView() const
    {
        return m_VelocityView;
    }
    Ref<ImageView> SceneRenderer::GetPunctualShadowView() const
    {
        return m_PunctualShadowView;
    }
}
