#pragma once

#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Veng.h>

#include <optional>
#include <vector>

namespace Veng
{
    class AssetManager;
    class MaterialInstance;
}

namespace Veng::Renderer
{
    class Context;
    class CommandBuffer;
    class Image;
    class ImageView;
    class Buffer;
    class GraphicsPipeline;
    struct SceneRendererSettings;

    /// @brief Owns the entity-id picking cluster and its request → stage → poll state machine.
    ///
    /// The picking vertical the renderer wires into its geometry timeframe: the R32Uint EntityId
    /// target and a dedicated depth buffer (allocated only while picking is enabled, so the shipping
    /// deferred path is byte-identical), the lazily-built static/skinned id-writing pipeline
    /// variants, the host-visible readback ring, and the requested/staged flags that drive the
    /// one-frame-late async readback. The renderer wires the picking passes inline in Rebuild (owning
    /// the graph), reading this subsystem's pipeline pointers and graph ids; the copy staging and the
    /// search-radius resolve live here.
    class PickingSystem
    {
    public:
        /// @brief Creates the picking subsystem (targets/pipelines are built lazily by Recreate/EnsurePipelines).
        /// @param context The render context the resources are created on.
        /// @param assets  Asset manager used to load the entity-id shaders.
        /// @return A new PickingSystem.
        static Unique<PickingSystem> Create(Context& context, AssetManager& assets);

        /// @brief Destroys the owned resources through the deferred-destruction retire path.
        ~PickingSystem();

        PickingSystem(const PickingSystem&) = delete;
        PickingSystem& operator=(const PickingSystem&) = delete;

        /// @brief Recreates the EntityId target + dedicated depth buffer, or releases them when off.
        ///
        /// Allocates the R32Uint EntityId target, its dedicated depth buffer, and the readback ring
        /// (all at @p extent) when Settings.Picking is set; otherwise releases any previously-created
        /// ones and drops the lazily-built pipelines. The id target's format is confirmed as a
        /// color-attachment + transfer source on the device before alloc. Called from the renderer's
        /// Create and every Resize/Configure.
        /// @param settings The active renderer settings (Picking gates allocation).
        /// @param extent   The allocation extent the targets are sized to.
        void Recreate(const SceneRendererSettings& settings, uvec2 extent);

        /// @brief Builds the static + skinned id-writing pipeline variants from representative materials.
        ///
        /// Reuses the surface material's pipeline layout + a dedicated entity-id vertex stage with the
        /// core entity_id fragment, binding the EntityId target + the dedicated depth format. Built
        /// lazily on the first Execute a surface material is available (the layout is shared across
        /// surface materials), cached thereafter. A no-op when picking is not allocated.
        /// @param staticMaterial  A loaded static surface material whose layout/vertex stage to reuse; may be null.
        /// @param skinnedMaterial A loaded skinned surface material whose layout/vertex stage to reuse; may be null.
        void EnsurePipelines(const MaterialInstance* staticMaterial,
                             const MaterialInstance* skinnedMaterial);

        /// @brief Whether the picking targets are currently allocated (picking is enabled).
        [[nodiscard]] bool IsAllocated() const { return m_EntityIdImage != nullptr; }

        /// @brief Records this Rebuild's EntityId and picking-depth import ids for Execute binding.
        /// @param entityIdId The R32Uint EntityId import id (or empty when not wired).
        /// @param depthId    The picking-depth import id (or empty when not wired).
        void SetGraphIds(ResourceId entityIdId, ResourceId depthId);

        /// @brief The R32Uint EntityId import id (the picking pass's color target).
        [[nodiscard]] ResourceId GetEntityIdId() const { return m_EntityIdId; }

        /// @brief The dedicated picking-depth import id.
        [[nodiscard]] ResourceId GetDepthId() const { return m_DepthId; }

        /// @brief Pointer to the lazily-built static id pipeline (borrowed by PickingScenePass).
        [[nodiscard]] const Ref<GraphicsPipeline>* GetStaticPipelinePointer() const
        {
            return &m_Pipeline;
        }

        /// @brief Pointer to the lazily-built skinned id pipeline (borrowed by PickingScenePass).
        [[nodiscard]] const Ref<GraphicsPipeline>* GetSkinnedPipelinePointer() const
        {
            return &m_SkinnedPipeline;
        }

        /// @brief Appends the EntityId + picking-depth import bindings when picking is active.
        /// @param bindings The renderer's per-frame import-binding list to append into.
        void AppendBindings(vector<RenderGraph::ImportBinding>& bindings) const;

        /// @brief Stages a pending pick's readback copy after the graph has run this Execute.
        ///
        /// When a pick is requested and not yet staged, transitions the EntityId target to
        /// TransferSrc and copies the search window under the cursor into the readback buffer on the
        /// graphics queue; the result becomes readable through PollPickId once this frame completes.
        /// @param cmd        Command buffer to record the copy into.
        /// @param extent     The allocation extent (bounds the search window).
        /// @param frameIndex This Execute's monotonic frame counter (the staged-frame stamp).
        void ServiceRequest(CommandBuffer& cmd, uvec2 extent, u64 frameIndex);

        /// @brief Records a pending pick at a render-target texel; a no-op when picking is off.
        /// @param texel The render-target texel to pick, in allocation pixels (top-left origin).
        void RequestPick(uvec2 texel);

        /// @brief Whether a pick request has been issued but not yet resolved or polled.
        [[nodiscard]] bool IsPickInFlight() const { return m_Requested; }

        /// @brief Resolves a requested pick's readback once ready, applying the search radius, else nullopt.
        ///
        /// The exact cursor texel wins when non-zero; otherwise the nearest non-zero id to the cursor.
        /// Returns the raw pick id (packed entity index + 1, or Picking::NoEntityId for background).
        /// Returns nullopt while the staged frame has not yet completed. Consuming the result clears
        /// the in-flight state.
        /// @param frameIndex The renderer's current monotonic frame counter (measures readback latency).
        /// @return The resolved pick id when ready; nullopt while the readback is still pending.
        [[nodiscard]] optional<u32> PollPickId(u64 frameIndex);

    private:
        PickingSystem(Context& context, AssetManager& assets);

        Context& m_Context;
        AssetManager& m_Assets;

        /// @brief Entity-id picking target (R32Uint), allocated only when Settings.Picking is set.
        Ref<Image> m_EntityIdImage;
        /// @brief View over m_EntityIdImage.
        Ref<ImageView> m_EntityIdView;
        /// @brief Dedicated depth buffer for the picking pass (so the nearest surface wins).
        Ref<Image> m_DepthImage;
        /// @brief View over m_DepthImage.
        Ref<ImageView> m_DepthView;

        /// @brief Imported id for the EntityId target in the internal graph.
        ResourceId m_EntityIdId;
        /// @brief Imported id for the picking depth buffer in the internal graph.
        ResourceId m_DepthId;

        /// @brief Static id-writing pipeline; built lazily on first Execute with a material.
        Ref<GraphicsPipeline> m_Pipeline;
        /// @brief Skinned id-writing pipeline; built lazily on first Execute with a material.
        Ref<GraphicsPipeline> m_SkinnedPipeline;

        /// @brief Host-visible staging buffer the picking readback copies the search window into.
        Ref<Buffer> m_ReadbackBuffer;
        /// @brief Stride in bytes of the readback window ((2*SearchRadius+1)² u32s).
        u32 m_ReadbackStride = 0;

        /// @brief Whether a pick request is awaiting service (RequestPick) or readback completion.
        bool m_Requested = false;
        /// @brief Whether a requested pick's region copy has been recorded and is awaiting GPU completion.
        bool m_Staged = false;
        /// @brief The requested texel (allocation pixels) the search window centers on.
        uvec2 m_Texel{};
        /// @brief Top-left texel of the staged search window (clamped into the target).
        uvec2 m_WindowOrigin{};
        /// @brief Cursor offset within the staged window (m_Texel - m_WindowOrigin).
        uvec2 m_CursorInWindow{};
        /// @brief Texel dimensions of the staged window (clamped to the target).
        uvec2 m_WindowExtent{};
        /// @brief Execute count at which the staged copy was recorded; the readback waits frames-in-flight.
        u64 m_StagedFrame = 0;
    };
}
