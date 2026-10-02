#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Veng.h>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class Buffer;
    class DescriptorSet;
    class DescriptorSetLayout;
    class ComputePipeline;
    class PipelineLayout;

    /// @brief The view-block LightTiles.x of a view no tile cull ran for: every light applies to
    ///        every pixel. Mirrors LightTilesNone in Veng/light_tiles.slang.
    inline constexpr u32 LightTilesNone = ~0u;

    /// @brief The tile grid covering an extent: LightTileSize-pixel tiles, the last row and column
    ///        partial when the extent is not a multiple of it.
    /// @param extent The extent in pixels.
    /// @return The grid's size in tiles; zero along an axis of zero extent.
    [[nodiscard]] constexpr uvec2 LightTileGrid(const uvec2 extent)
    {
        return {(extent.x + LightTileSize - 1) / LightTileSize,
                (extent.y + LightTileSize - 1) / LightTileSize};
    }

    /// @brief Owns the per-tile light-mask cull: the mask ring, its bindless slot, and the cull pass.
    ///
    /// The lighting pass loops every packed light at every pixel; most lights reach only part of the
    /// screen. Ahead of it, one compute workgroup per LightTileSize-square tile reduces the tile's
    /// depth to its nearest and farthest geometry, bounds that slice of the tile's frustum by a
    /// view-space box, and writes one 32-bit mask of the lights whose influence sphere reaches the
    /// box (light_tile_cull.comp). The lighting pass then visits only its tile's lights. The test is
    /// conservative, so the image is unchanged; only the work moves.
    ///
    /// The masks live in a device-local storage buffer with one region per frame in flight — a
    /// renderer executes once per frame, so the region a frame writes is never one a frame still on
    /// the GPU reads — registered in the set-0 storage-buffer array so any pass can find this
    /// frame's masks through the view block (ViewState). The buffer is imported into the graph, so
    /// the write-to-read barrier between the cull and the lighting pass is graph-derived.
    class LightTileCuller
    {
    public:
        /// @brief Creates the cull pipeline and its set layout. Allocates no masks until Recreate.
        /// @param context The render context the resources are created on.
        /// @param assets  Asset manager used to load the cull compute shader.
        /// @return A new LightTileCuller.
        static Unique<LightTileCuller> Create(Context& context, AssetManager& assets);

        /// @brief Releases the mask buffer's bindless slot; the buffer retires deferred.
        ~LightTileCuller();

        LightTileCuller(const LightTileCuller&) = delete;
        LightTileCuller& operator=(const LightTileCuller&) = delete;

        /// @brief Sizes the mask ring for a render allocation, or releases it.
        ///
        /// The grid covers the whole allocation, so a dynamic-resolution sub-rect inside it needs no
        /// reallocation. Recreates the buffer, its bindless slot, and the cull set when the grid
        /// changes; a no-op when it does not.
        /// @param enabled          Whether the cull may run (Settings.LightTileCulling).
        /// @param renderAllocExtent The render allocation the lighting pass shades.
        void Recreate(bool enabled, uvec2 renderAllocExtent);

        /// @brief Whether a mask ring is allocated.
        [[nodiscard]] bool IsAllocated() const { return m_Buffer != nullptr; }

        /// @brief Imports the mask buffer into the graph being rebuilt, when the cull is wired.
        /// @param graph  The renderer's internal graph being rebuilt.
        /// @param active Whether this topology wires the cull.
        /// @return The import's id, or an invalid id when inactive.
        ResourceId Import(RenderGraph& graph, bool active);

        /// @brief The mask buffer's import id from the last Import (invalid when inactive).
        [[nodiscard]] ResourceId GetMaskId() const { return m_MaskId; }

        /// @brief The mask buffer, bound to its import each Execute.
        [[nodiscard]] const Ref<Buffer>& GetBuffer() const { return m_Buffer; }

        /// @brief Declares the cull compute pass, which reads the g-buffer depth and writes this
        ///        frame's masks.
        /// @param graph       The renderer's internal graph being rebuilt.
        /// @param depthId     The g-buffer depth the pass reduces.
        /// @param depthHandle The depth's bindless slot, through which the shader reads it.
        void DeclareCull(RenderGraph& graph, ResourceId depthId, TextureHandle depthHandle);

        /// @brief The view block's LightTiles word for this frame.
        /// @param active     Whether the cull runs for this view this frame.
        /// @param frameSlot  The frame-in-flight index selecting the mask region.
        /// @return The bindless slot, the grid's row stride, and the region's first word — or
        ///         LightTilesNone in x when inactive, so a reader applies every light.
        [[nodiscard]] uvec4 ViewState(bool active, u32 frameSlot) const;

        /// @brief Reads back the masks the last recorded cull wrote. Blocks on a device read.
        /// @return The masks over the tile grid of the extent the cull covered, or empty when no
        ///         cull has been recorded.
        [[nodiscard]] LightTileMasks Readback() const;

    private:
        LightTileCuller(Context& context, AssetManager& assets);

        Context& m_Context;

        /// @brief The cull compute pipeline (depth + lights → per-tile masks).
        Ref<ComputePipeline> m_Pipeline;
        /// @brief Layout for the cull pipeline: the set-3 mask buffer + the push block.
        Ref<PipelineLayout> m_Layout;
        /// @brief Set-3 layout: the mask buffer as a writable storage buffer (binding 0).
        Ref<DescriptorSetLayout> m_SetLayout;
        /// @brief The cull's set binding the current mask buffer.
        Ref<DescriptorSet> m_Set;

        /// @brief The mask ring: one region of m_Grid.x * m_Grid.y words per frame in flight.
        Ref<Buffer> m_Buffer;
        /// @brief The mask buffer's slot in the set-0 storage-buffer array.
        StorageBufferHandle m_Handle;
        /// @brief The tile grid of the render allocation the ring is sized for.
        uvec2 m_Grid{0};
        /// @brief Frames in flight the ring holds a region for.
        u32 m_FramesInFlight = 0;

        /// @brief The mask buffer's import id this topology.
        ResourceId m_MaskId;

        /// @brief The region the last recorded cull wrote.
        u32 m_LastRegion = 0;
        /// @brief The tile grid of the extent the last recorded cull covered (zero before any).
        uvec2 m_LastTiles{0};
    };
}
