#include "PickingSystem.h"

#include "Picking.h"

#include <algorithm>

#include <Veng/Assert.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneRenderer.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Shader.h>

namespace Veng::Renderer
{
    namespace
    {
        // The entity-id picking shaders: a minimal vertex stage (static + skinned) that emits the
        // per-draw entity index, and the fragment that writes index + 1 into the EntityId target.
        constexpr AssetId EntityIdFragId{0xBE08B2489A5AA07AULL};
        constexpr AssetId EntityIdVertId{0xE21B8F492DADABE5ULL};
        constexpr AssetId EntityIdSkinnedVertId{0x7FB330D3ABACAE0FULL};
    }

    Unique<PickingSystem> PickingSystem::Create(Context& context, AssetManager& assets)
    {
        return Unique<PickingSystem>(new PickingSystem(context, assets));
    }

    PickingSystem::PickingSystem(Context& context, AssetManager& assets)
        : m_Context(context), m_Assets(assets)
    {
    }

    PickingSystem::~PickingSystem() = default;

    void PickingSystem::Recreate(const SceneRendererSettings& settings, const uvec2 extent)
    {
        if (!settings.Picking)
        {
            // Release any previously-allocated picking resources (a Configure turning it off);
            // the pipelines are rebuilt lazily, so they are dropped here too.
            m_EntityIdImage.reset();
            m_EntityIdView.reset();
            m_DepthImage.reset();
            m_DepthView.reset();
            m_ReadbackBuffer.reset();
            m_Pipeline.reset();
            m_SkinnedPipeline.reset();
            m_Requested = false;
            m_Staged = false;
            return;
        }

        VE_ASSERT(
            m_Context.IsFormatColorAttachmentTransferSrcSupported(Picking::EntityIdFormat),
            "SceneRenderer: the device does not support R32Uint as a color attachment + transfer "
            "source, required for the entity-id picking target");

        m_EntityIdImage = Image::Create(m_Context, {
                                                       .Name = "SceneRenderer EntityId",
                                                       .Extent = {extent.x, extent.y, 1},
                                                       .Format = Picking::EntityIdFormat,
                                                       .Usage = Picking::EntityIdUsage,
                                                   });
        m_EntityIdView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer EntityId View", .Image = m_EntityIdImage});

        m_DepthImage = Image::Create(m_Context, {
                                                    .Name = "SceneRenderer Picking Depth",
                                                    .Extent = {extent.x, extent.y, 1},
                                                    .Format = GBuffer::DepthFormat,
                                                    .Usage = ImageUsage::DepthAttachment,
                                                });
        m_DepthView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer Picking Depth View", .Image = m_DepthImage});

        // The readback staging buffer: one search-window's worth of u32s. Only one pick is in
        // flight at a time (a new request is dropped until the prior resolves), so the staged copy
        // is never overwritten before the host reads it — one region suffices.
        const u32 window = static_cast<u32>(2 * Picking::SearchRadius + 1);
        m_ReadbackStride = window * window * static_cast<u32>(sizeof(u32));
        m_ReadbackBuffer = Buffer::Create(m_Context, {
                                                         .Name = "SceneRenderer Pick Readback",
                                                         .Size = m_ReadbackStride,
                                                         .Usage = BufferUsage::TransferDst,
                                                         .HostMapped = true,
                                                     });

        // A resize/configure recreates the id target, so an in-flight staged copy is moot.
        m_Staged = false;
    }

    void PickingSystem::EnsurePipelines(const MaterialInstance* staticMaterial,
                                        const MaterialInstance* skinnedMaterial)
    {
        if (!IsAllocated())
        {
            return;
        }

        // The id-writing variants reuse the surface material's pipeline layout (set 0 bindless +
        // set 1 DrawData [+ set 2 palette for skinned] + the SurfacePush) so the picking pass binds
        // the same per-draw DrawData and palette the geometry pass does. They pair the dedicated
        // entity_id vertex stages (which emit only the entity index) with the entity_id fragment,
        // and bind the EntityId target + dedicated depth instead of the g-buffer. The layout is
        // identical across all surface materials, so the first available one builds the cached pipeline.
        auto LoadShader = [this](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result =
                m_Assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "SceneRenderer: {} shader load failed: {}", what,
                      result.has_value() ? "" : result.error().Detail);
            return *result;
        };

        if (!m_Pipeline && staticMaterial != nullptr)
        {
            const AssetHandle<Veng::Shader> vs = LoadShader(EntityIdVertId, "entity-id vertex");
            const AssetHandle<Veng::Shader> fs = LoadShader(EntityIdFragId, "entity-id fragment");
            m_Pipeline = GraphicsPipeline::Create(
                m_Context, {
                               .Name = "SceneRenderer Picking Pipeline",
                               .ColorAttachments = {{.Format = Picking::EntityIdFormat}},
                               .DepthAttachmentFormat = GBuffer::DepthFormat,
                               .VertexBufferLayout = Mesh::CanonicalLayout(),
                               .InstanceCandidateId = true,
                               .PipelineLayout = staticMaterial->GetPipelineLayout(),
                               .ShaderStages =
                                   {
                                       {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                       {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                                   },
                               .CullMode = CullMode::Back,
                               .DepthTestEnable = true,
                               .DepthWriteEnable = true,
                               // Reverse-Z: a nearer fragment has larger depth.
                               .DepthCompareOp = CompareOp::GreaterOrEqual,
                           });
        }

        // The skinned id pipeline reuses the material's skinned layout (set 0 bindless, set 1
        // DrawData, set 2 the palette), not its static two-set layout: the picking pass binds the
        // palette at set 2 exactly as the g-buffer pass does, so the layout must carry that set.
        if (!m_SkinnedPipeline && skinnedMaterial != nullptr &&
            skinnedMaterial->GetSkinnedPipelineLayout() != nullptr)
        {
            const AssetHandle<Veng::Shader> vs =
                LoadShader(EntityIdSkinnedVertId, "entity-id skinned vertex");
            const AssetHandle<Veng::Shader> fs = LoadShader(EntityIdFragId, "entity-id fragment");
            m_SkinnedPipeline = GraphicsPipeline::Create(
                m_Context, {
                               .Name = "SceneRenderer Picking Skinned Pipeline",
                               .ColorAttachments = {{.Format = Picking::EntityIdFormat}},
                               .DepthAttachmentFormat = GBuffer::DepthFormat,
                               .VertexBufferLayout = Mesh::SkinnedLayout(),
                               .InstanceCandidateId = true,
                               .PipelineLayout = skinnedMaterial->GetSkinnedPipelineLayout(),
                               .ShaderStages =
                                   {
                                       {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                       {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                                   },
                               .CullMode = CullMode::Back,
                               .DepthTestEnable = true,
                               .DepthWriteEnable = true,
                               // Reverse-Z: a nearer fragment has larger depth.
                               .DepthCompareOp = CompareOp::GreaterOrEqual,
                           });
        }
    }

    void PickingSystem::SetGraphIds(const ResourceId entityIdId, const ResourceId depthId)
    {
        m_EntityIdId = entityIdId;
        m_DepthId = depthId;
    }

    void PickingSystem::AppendBindings(vector<RenderGraph::ImportBinding>& bindings) const
    {
        if (!IsAllocated())
        {
            return;
        }
        bindings.push_back({m_EntityIdId, m_EntityIdView});
        bindings.push_back({m_DepthId, m_DepthView});
    }

    void PickingSystem::ServiceRequest(CommandBuffer& cmd, const uvec2 extent, const u64 frameIndex)
    {
        // Service a pending pick: the picking pass left the EntityId target in ColorAttachment
        // layout, so transition it to TransferSrc and copy the search window under the cursor into
        // the readback region. The result becomes readable once this frame's GPU work completes
        // (PollPickId waits frames-in-flight); the copy rides the graphics queue, no stall.
        if (!m_Requested || m_Staged || !IsAllocated())
        {
            return;
        }

        const u32 window = static_cast<u32>(2 * Picking::SearchRadius + 1);
        // Clamp the window into the target; the cursor's offset within it is recorded so the
        // search-radius logic measures distance from the real cursor texel, not the clamped one.
        const uvec2 maxOrigin = uvec2(extent.x - 1, extent.y - 1);
        const uvec2 clampedTexel = glm::min(m_Texel, maxOrigin);
        const uvec2 origin{
            clampedTexel.x >= static_cast<u32>(Picking::SearchRadius)
                ? std::min(clampedTexel.x - static_cast<u32>(Picking::SearchRadius),
                           extent.x - window)
                : 0u,
            clampedTexel.y >= static_cast<u32>(Picking::SearchRadius)
                ? std::min(clampedTexel.y - static_cast<u32>(Picking::SearchRadius),
                           extent.y - window)
                : 0u,
        };
        const uvec2 copyExtent{std::min(window, extent.x), std::min(window, extent.y)};

        cmd.PrepareForAccess(m_EntityIdView, AccessKind::TransferSrc);
        // The copy lands tightly packed at buffer byte 0 as a copyExtent.x-wide grid; the host
        // reads it once this frame completes (PollPickId waits frames-in-flight).
        cmd.CopyImageRegionToBuffer(m_EntityIdImage, m_ReadbackBuffer, origin, copyExtent);

        m_WindowOrigin = origin;
        m_WindowExtent = copyExtent;
        m_CursorInWindow = clampedTexel - origin;
        m_Staged = true;
        m_StagedFrame = frameIndex;
    }

    void PickingSystem::RequestPick(const uvec2 texel)
    {
        if (!IsAllocated())
        {
            return;
        }
        // A new request replaces any in-flight one (the latest click wins).
        m_Texel = texel;
        m_Requested = true;
        m_Staged = false;
    }

    optional<u32> PickingSystem::PollPickId(const u64 frameIndex)
    {
        if (!m_Requested || !m_Staged)
        {
            return std::nullopt;
        }
        // The staged copy is safe to read once its frame's GPU work has retired — at least
        // GetMaxFramesInFlight() Executes after it was recorded (the same deferral the retire
        // path uses). Until then the readback is still pending.
        if (frameIndex - m_StagedFrame < m_Context.GetMaxFramesInFlight())
        {
            return std::nullopt;
        }

        // Search the staged window: the exact cursor texel wins when non-zero; otherwise the
        // nearest non-zero id to the cursor (front-most resolution rides the depth test the
        // picking pass already applied, so a non-zero texel is already the nearest surface there).
        const auto* ids = static_cast<const u32*>(m_ReadbackBuffer->GetMappedData());
        const u32 width = m_WindowExtent.x;
        const u32 height = m_WindowExtent.y;

        u32 resolved = Picking::NoEntityId;
        const uvec2 cursor = m_CursorInWindow;
        const u32 exact = (cursor.x < width && cursor.y < height) ? ids[cursor.y * width + cursor.x]
                                                                  : Picking::NoEntityId;
        if (exact != Picking::NoEntityId)
        {
            resolved = exact;
        }
        else
        {
            u64 bestDistanceSq = ~0ull;
            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    const u32 id = ids[y * width + x];
                    if (id == Picking::NoEntityId)
                    {
                        continue;
                    }
                    const i64 dx = static_cast<i64>(x) - static_cast<i64>(cursor.x);
                    const i64 dy = static_cast<i64>(y) - static_cast<i64>(cursor.y);
                    const u64 distanceSq = static_cast<u64>(dx * dx + dy * dy);
                    if (distanceSq < bestDistanceSq)
                    {
                        bestDistanceSq = distanceSq;
                        resolved = id;
                    }
                }
            }
        }

        // Consume the request: the caller takes the result.
        m_Requested = false;
        m_Staged = false;
        return resolved;
    }
}
