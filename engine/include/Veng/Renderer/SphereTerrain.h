#pragma once

#include <array>
#include <memory>
#include <span>

#include <Veng/Veng.h>
#include <Veng/Math/CubeSphere.h>
#include <Veng/Renderer/Types.h>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;
    class Image;
    class ImageView;
    class SphereTerrainResidency;

    /// @brief The most channels a sphere terrain's tile payload may carry.
    inline constexpr u32 MaxSphereTerrainChannels = 8;

    /// @brief The cells across a tile: its interior spans this many texel steps.
    inline constexpr u32 SphereTerrainTileCells = 128;

    /// @brief The border texels on each side of a tile, for filtering and finite differences.
    inline constexpr u32 SphereTerrainTileBorder = 2;

    /// @brief The texels a side of a full-resolution tile channel: the cells plus one, so a tile's
    ///        edge row is shared with its neighbour's, plus the border on both sides.
    inline constexpr u32 SphereTerrainTileTexels =
        SphereTerrainTileCells + 1 + (2 * SphereTerrainTileBorder);

    /// @brief The quads a side of the grid a tile is drawn with; vertex `k` sits on texel
    ///        `Border + k · Cells / GridQuads`.
    inline constexpr u32 SphereTerrainGridQuads = 32;

    /// @brief The layer index meaning "no layer": a level-0 tile's parent.
    inline constexpr u32 SphereTerrainNoLayer = ~0u;

    /// @brief The most 32-bit words of per-tile data a generator hands the drawing stages, for a
    ///        tile and again for its parent.
    inline constexpr u32 MaxSphereTerrainTileWords = 32;

    /// @brief The most level layers a terrain draws over its own surface.
    inline constexpr u32 MaxSphereTerrainLevelLayers = 2;

    /// @brief The texels a side of a channel held at a divisor of the full tile resolution.
    /// @param divisor The channel's TexelDivisor; it divides the tile's span of texels exactly.
    [[nodiscard]] constexpr u32 SphereTerrainChannelTexels(const u32 divisor)
    {
        return ((SphereTerrainTileTexels - 1) / divisor) + 1;
    }

    /// @brief One channel of a sphere terrain's tile payload.
    struct SphereTerrainChannelInfo
    {
        /// @brief Debug name; the channel's pool image is named after it.
        string Name;
        /// @brief The texel format the channel is stored in.
        Format TexelFormat = Format::R32Sfloat;
        /// @brief The fraction of the full tile resolution the channel holds, as a divisor.
        ///
        /// A channel at divisor `n` holds `(TileTexels − 1) / n + 1` texels a side over the same
        /// patch, so a coarse channel (a horizon, a mask) costs a fraction of a full one. It must
        /// divide `TileTexels − 1` (132): 1, 2, 3, 4, 6, 11 or 12.
        u32 TexelDivisor = 1;
    };

    /// @brief One tile's placement, packed for the GPU; mirrors `SphereTerrainTileConstants` in
    ///        `Veng/sphere_terrain_mapping.slang` byte for byte (128 bytes, so it fits the push
    ///        constant range every Vulkan device guarantees).
    ///
    /// A texel's body-fixed direction is `CenterHigh + (CenterLow + offset)`, with the offset from
    /// `SphereTerrainTileOffset`; the offset alone is accurate to the f32 rounding of the tile's own
    /// size, which is what a kernel working at depth keeps separate from the centre.
    struct SphereTerrainTileConstants
    {
        /// @brief The centre direction rounded to f32 (xyz), and the sphere's radius in metres (w).
        vec4 CenterHigh{0.0f};
        /// @brief The centre direction's double-precision residual (xyz); w is unused.
        vec4 CenterLow{0.0f};
        /// @brief Per corner, the offset from the centre direction (xyz) and the cube-point norm (w).
        std::array<vec4, 4> Corners{};
        /// @brief The cube-plane starts and warp spans: (x0u, Δθ, x0v, Δθ).
        vec4 Warp{0.0f};
        /// @brief The face in bits 0–7 and the level in 8–15 (x), the tile's column (y) and row (z),
        ///        and the layout (w): TileTexels in bits 0–7, Border in 8–15, Cells in 16–31.
        uvec4 Tile{0u};
    };
    static_assert(sizeof(SphereTerrainTileConstants) == 128,
                  "SphereTerrainTileConstants must match the shader's 128-byte layout");

    /// @brief Packs a tile's GPU constants.
    /// @param key     The tile.
    /// @param radius  The sphere's radius in metres.
    [[nodiscard]] VE_API SphereTerrainTileConstants
    PackSphereTerrainTileConstants(CubeSphereTileKey key, f64 radius);

    /// @brief Everything a generator needs to fill one tile.
    ///
    /// Handed to SphereTerrainGenerator::RecordTile from a generated-texture tick. The channel views
    /// are storage views over the tile's own layer of each channel, already in a storage layout.
    /// The parent's views are single-layer sampled views over the parent's layer, which is resident
    /// and sampleable; the pool's whole-array views are not handed over, since this tile's layer is
    /// in a storage layout while the tick runs and a sampled descriptor over the whole array would
    /// name it in the wrong layout.
    struct SphereTerrainTileWork
    {
        /// @brief The tile.
        CubeSphereTileKey Key;
        /// @brief The tile's layer in every channel's pool array.
        u32 Layer = 0;
        /// @brief The tile's placement in double, for a consumer that reduces its own domain on the
        ///        host (a lattice offset, a precision anchor).
        CubeSphereTileGeometry Geometry;
        /// @brief The sphere's radius in metres.
        f64 Radius = 1.0;
        /// @brief The tile's GPU constants, ready to push or upload.
        SphereTerrainTileConstants Constants;
        /// @brief One storage view per channel, over this tile's layer, in channel order.
        std::span<const Ref<ImageView>> ChannelViews;
        /// @brief The parent's layer, or SphereTerrainNoLayer at level 0.
        u32 ParentLayer = SphereTerrainNoLayer;
        /// @brief One sampled view per channel over the parent's layer; empty at level 0.
        std::span<const Ref<ImageView>> ParentViews;
    };

    /// @brief The consumer's half of a sphere terrain: what a tile holds, and the kernels that fill
    ///        it.
    ///
    /// A consumer subclasses it, and it is the only place a consumer's kernels enter.
    ///
    /// **The contract: a texel's value is a function of its direction and its level alone.** Two
    /// tiles of one level then agree on the texels they share along an edge, and on the border
    /// texels each holds of the other, up to the f32 rounding of their own anchors. Crack-free and
    /// seam-free drawing rests on it.
    ///
    /// **Channel 0 is height**, `R32Sfloat` at divisor 1, in metres above the source's radius. The
    /// engine measures each tile's range of it. The other channels are the consumer's.
    class SphereTerrainGenerator
    {
    public:
        /// @brief Virtual for subclassing.
        virtual ~SphereTerrainGenerator() = default;

        /// @brief The tile payload: at most MaxSphereTerrainChannels channels, height first.
        [[nodiscard]] virtual vector<SphereTerrainChannelInfo> GetChannels() const = 0;

        /// @brief Records the dispatches that fill one tile's layer of every channel.
        ///
        /// Called from a generated-texture tick on the render thread, with the frame's command
        /// buffer. It may record any number of dispatches. For a pinned level it may resample maps
        /// the consumer already has rather than evaluate a kernel.
        /// @param cmd   The command buffer to record into.
        /// @param work  The tile and the views to write.
        virtual void RecordTile(CommandBuffer& cmd, const SphereTerrainTileWork& work) = 0;

        /// @brief The generated-texture cost of one tile at a level; one by default.
        [[nodiscard]] virtual u32 GetTileCost(u32 level) const
        {
            (void)level;
            return 1;
        }

        /// @brief A string naming everything a tile's texels depend on beyond its key.
        ///
        /// Non-empty, it keys tiles into the generated-texture cache, so a tile generated on an
        /// earlier run is restored rather than regenerated. Empty — the default — caches nothing.
        [[nodiscard]] virtual string GetCacheSalt() const { return {}; }

        /// @brief The most a tile's height range can exceed its parent's at a level, in metres.
        ///
        /// A tile not yet measured is bounded by its parent's measured range widened by this, so
        /// the bound a generator gives decides how early an unmeasured tile splits. A sphere-wide
        /// bound would put every tile near the eye within reach and split everything near it.
        [[nodiscard]] virtual f32 GetLevelHeightBound(u32 level) const = 0;

        /// @brief Writes the per-tile words the drawing stages read for a tile.
        ///
        /// For shading that needs per-tile constants the engine does not carry — a precision
        /// anchor, a lattice offset reduced on the host. Called on the render thread the first
        /// frame a tile is drawn or is a drawn tile's parent, and cached while it stays so; the
        /// fragment reads the words through `TerrainTileData` and `TerrainParentTileData`
        /// (`Veng/sphere_terrain.slang`). The words must be a function of the key alone. Writes
        /// nothing by default, which leaves them zero.
        /// @param key    The tile.
        /// @param words  MaxSphereTerrainTileWords words, zeroed.
        virtual void WriteTileDrawData(CubeSphereTileKey key, std::span<u32> words) const
        {
            (void)key;
            (void)words;
        }
    };

    /// @brief Creation parameters for a SphereTerrainSource.
    struct SphereTerrainSourceInfo
    {
        /// @brief Debug name for the pool images and the generated-texture jobs.
        string Name = "SphereTerrain";
        /// @brief The generator; required.
        Ref<SphereTerrainGenerator> Generator;
        /// @brief The sphere's radius in metres.
        f64 Radius = 1.0;
        /// @brief The lowest value channel 0 takes anywhere on the sphere, in metres.
        f32 MinHeight = 0.0f;
        /// @brief The highest value channel 0 takes anywhere on the sphere, in metres.
        f32 MaxHeight = 0.0f;
        /// @brief The pool's capacity in tiles; clamped to the device's array-layer limit.
        u32 MaxResidentTiles = 512;
        /// @brief The deepest level generated at creation and never evicted.
        ///
        /// Levels `0..PinnedLevel` hold `2 · (4^(PinnedLevel+1) − 1)` tiles — 126 at the default of
        /// two — and end every parent chain at a resident tile.
        u32 PinnedLevel = 2;
        /// @brief The deepest level generated: the level whose texels reach the finest band the
        ///        generator carries.
        u32 MaxLevel = CubeSphere::MaxLevel;
        /// @brief The most tiles requested but not yet generated at once.
        ///
        /// A request beyond it waits and is counted as deferred; pending tiles hold pool layers, so
        /// the bound keeps a fast-moving eye from filling the pool with work it may no longer want.
        u32 MaxPendingTiles = 64;
    };

    /// @brief What a sphere terrain's pool holds, for a diagnostic panel.
    struct SphereTerrainSourceStats
    {
        /// @brief Tiles generated and resident.
        u32 ResidentTiles = 0;
        /// @brief Resident tiles in the pinned top.
        u32 PinnedTiles = 0;
        /// @brief Tiles requested and still generating.
        u32 PendingTiles = 0;
        /// @brief The pool's capacity in tiles.
        u32 Capacity = 0;
        /// @brief The pool's memory, every channel's array.
        u64 Bytes = 0;
        /// @brief Resident tiles evicted, over the source's lifetime.
        u64 Evictions = 0;
        /// @brief Tiles requested, over the source's lifetime.
        u64 Requests = 0;
        /// @brief Requests that waited on a full pool or the pending bound, over the lifetime.
        u64 RequestsDeferred = 0;
        /// @brief Tiles whose generator ran in the last frame's generation.
        u32 TilesGeneratedLastFrame = 0;
        /// @brief Tiles restored from the cache in the last frame's generation.
        u32 TilesRestoredLastFrame = 0;
        /// @brief Tiles generated over the source's lifetime.
        u64 TilesGeneratedTotal = 0;
        /// @brief Tiles restored from the cache over the source's lifetime.
        u64 TilesRestoredTotal = 0;
    };

    /// @brief A resident tile and what the engine measured of it.
    struct SphereTerrainResidentTile
    {
        /// @brief The tile.
        CubeSphereTileKey Key;
        /// @brief Its layer in every channel's pool array.
        u32 Layer = 0;
        /// @brief Whether the bound below was measured; false until the reduction lands.
        bool Measured = false;
        /// @brief The lowest height over the tile's interior, in metres.
        f32 MinHeight = 0.0f;
        /// @brief The highest height over the tile's interior, in metres.
        f32 MaxHeight = 0.0f;
        /// @brief The largest difference, over the tile's grid vertices, between the tile's height
        ///        and its parent's grid interpolated there, in metres.
        f32 MorphError = 0.0f;
    };

    /// @brief One tile a renderer drew, with what its draw needs to blend into its parent.
    struct SphereTerrainDrawnTile
    {
        /// @brief The tile.
        CubeSphereTileKey Key;
        /// @brief Its layer in every channel's pool array.
        u32 Layer = 0;
        /// @brief Its parent's layer, or SphereTerrainNoLayer at level 0.
        u32 ParentLayer = SphereTerrainNoLayer;
        /// @brief The working height range: the union of the tile's range and its parent's.
        f32 MinHeight = 0.0f;
        /// @brief The working height range's top.
        f32 MaxHeight = 0.0f;
        /// @brief The eye distance at which the tile starts morphing into its parent's shape.
        f32 MorphStart = 0.0f;
        /// @brief The eye distance at which the tile is wholly its parent's shape.
        f32 MorphEnd = 0.0f;
        /// @brief The largest difference between the tile's height and its parent's grid, in
        ///        metres: measured, or the generator's level bound while unmeasured.
        f32 MorphError = 0.0f;
        /// @brief Whether the tile wanted to split and was held whole: its children still
        ///        generating, the selection's tile budget spent, or the view frustum pruning it.
        ///
        /// A held tile is drawn nearer than its level's range, so a finer neighbour meets it
        /// partly morphed rather than fully; the skirts cover the step, which is at most that
        /// neighbour's MorphError.
        bool Held = false;
    };

    /// @brief The selection rule's tunables; the defaults are the shipped values.
    struct SphereTerrainSelectionSettings
    {
        /// @brief The relative band around a level's range in which a tile neither splits nor
        ///        merges.
        f32 Hysteresis = 0.05f;
        /// @brief Where a tile's morph starts, as a fraction of its morph end.
        f32 MorphStartRatio = 0.85f;
        /// @brief The floor on a level's range, in tile diameters, which holds the neighbour
        ///        invariant at any field of view.
        f32 PlanarRangeFloor = 3.0f;
        /// @brief The relief, in pixels at the tile's distance, its parent's grid may misplace
        ///        before the tile splits.
        f32 TargetHeightErrorPixels = 1.0f;
        /// @brief How far past a drawn tile's split distance its children are prefetched, as a
        ///        multiple of it.
        f32 PrefetchRangeScale = 1.3f;
        /// @brief The radius about the eye, in metres, inside which tiles refine in every
        ///        direction; beyond it only tiles meeting the view frustum refine.
        ///
        /// Turning the head therefore never strips the ground near the eye, while the far field
        /// behind the view stays at the levels its parents give it. A renderer raises it to its
        /// shadow distance, so every tile a shadow cascade receives on is refined whichever way
        /// the eye looks.
        f32 OmnidirectionalRadius = 64.0f;
    };

    /// @brief What one renderer's selection of one sphere terrain did last frame.
    struct SphereTerrainSelectionStats
    {
        /// @brief Tiles drawn.
        u32 DrawnTiles = 0;
        /// @brief Tiles drawn, per level.
        std::array<u32, CubeSphere::MaxLevel + 1> DrawnPerLevel{};
        /// @brief Drawn tiles that wanted to split but had a child still generating.
        u32 WaitingSplits = 0;
        /// @brief Drawn tiles that wanted to split but were held whole to keep the drawn set 2:1
        ///        balanced: a tile their children would border could not be split, its own
        ///        children still generating.
        u32 HeldForBalance = 0;
        /// @brief Tiles split only to keep the drawn set 2:1 balanced: past the horizon, beside a
        ///        refined neighbour.
        u32 SplitForBalance = 0;
        /// @brief Drawn tiles that wanted to split but were held whole by the tile budget.
        u32 HeldForBudget = 0;
        /// @brief Drawn tiles that wanted to split but lie outside the view frustum beyond the
        ///        omnidirectional radius.
        u32 HeldForFrustum = 0;
        /// @brief Tiles the selection wanted kept or generated, beyond the pinned top: what it
        ///        charged against its tile budget.
        u32 BudgetedTiles = 0;
        /// @brief The tile budget the selection ran under; ~0 for none.
        u32 TileBudget = ~0u;
        /// @brief Tiles the main view drew, its level layers' draws included.
        u32 MainViewTiles = 0;
        /// @brief Selected tiles the main view culled by its frustum.
        u32 CulledByFrustum = 0;
        /// @brief Selected tiles the main view culled below the horizon.
        u32 CulledByHorizon = 0;
        /// @brief Tiles drawn into the shadow views, summed over the views.
        u32 ShadowViewTiles = 0;
        /// @brief Triangles drawn over every view, skirts included.
        u64 Triangles = 0;
        /// @brief Drawn tiles still morphing in under an arrival floor.
        u32 ArrivingTiles = 0;
        /// @brief Terrains skipped because their material does not declare "sphereTerrain" or
        ///        is not resident.
        u32 SkippedTerrains = 0;
        /// @brief Tiles requested.
        u32 Requests = 0;
        /// @brief Quadtree nodes the selection visited.
        u32 Visited = 0;
    };

    /// @brief The terrain of one sphere: a cube-sphere quadtree of tiles, generated on the GPU by a
    ///        consumer's kernels into a pooled tile atlas.
    ///
    /// The pool is one array image per channel, `capacity` layers each, with one layer index per
    /// tile shared across the channels; it binds through a descriptor set of its own and never
    /// touches the bindless registry, so its size is bounded only by memory and the device's layer
    /// limit. Each tile is one GeneratedTextureService job adopting its layer of every channel, so
    /// generation is budgeted by the frame's GPU headroom and may be answered from the disk cache.
    ///
    /// **Residency is closed under parent.** A tile is requested only when its parent is resident,
    /// and a tile is evicted only when it has no resident or pending child, is not pinned, and was
    /// not wanted this frame or the last by any renderer. Eviction takes the tile wanted least
    /// recently. The pinned top (levels `0..PinnedLevel`) is requested at creation, at the highest
    /// priority, and never evicted, so every parent chain ends at a resident tile and a renderer's
    /// parent fallback always terminates. When the pool is full and nothing is evictable a request
    /// waits and is counted; the pool never exceeds its capacity.
    ///
    /// **Bounds are measured.** After a tile's generator runs, an engine reduction measures its
    /// height range and MorphError; it is read back once the frame that ran it retires. Until then
    /// a tile is bounded by its parent's range widened by the generator's level bound.
    ///
    /// A source is shared: several renderers and several scenes may draw it, each marking what it
    /// wants and keeping its own selection, so a sphere moved between scenes keeps its tiles.
    /// Created by the consumer and held through the SphereTerrain component.
    class VE_API SphereTerrainSource
    {
    public:
        /// @brief Creates the pool, the reduction pipeline, and requests the pinned top.
        /// @param context  Render context.
        /// @param assets   Asset manager, for the engine's bounds reduction shader.
        /// @param info     The source's description.
        /// @return The source.
        [[nodiscard]] static Ref<SphereTerrainSource> Create(Context& context, AssetManager& assets,
                                                             const SphereTerrainSourceInfo& info);

        /// @brief Releases every job the source still holds.
        ~SphereTerrainSource();

        SphereTerrainSource(const SphereTerrainSource&) = delete;
        SphereTerrainSource& operator=(const SphereTerrainSource&) = delete;

        /// @brief A process-unique serial, which keys the source's generated-texture jobs and a
        ///        renderer's per-source selection memory.
        [[nodiscard]] u64 GetSerial() const;

        /// @brief The debug name.
        [[nodiscard]] const string& GetName() const;

        /// @brief The sphere's radius in metres.
        [[nodiscard]] f64 GetRadius() const;

        /// @brief The lowest height channel 0 takes anywhere, in metres.
        [[nodiscard]] f32 GetMinHeight() const;

        /// @brief The highest height channel 0 takes anywhere, in metres.
        [[nodiscard]] f32 GetMaxHeight() const;

        /// @brief The deepest pinned level.
        [[nodiscard]] u32 GetPinnedLevel() const;

        /// @brief The deepest level generated.
        [[nodiscard]] u32 GetMaxLevel() const;

        /// @brief The pool's capacity in tiles.
        [[nodiscard]] u32 GetCapacity() const;

        /// @brief The generator.
        [[nodiscard]] SphereTerrainGenerator& GetGenerator() const;

        /// @brief The number of payload channels.
        [[nodiscard]] u32 GetChannelCount() const;

        /// @brief One payload channel's description.
        [[nodiscard]] const SphereTerrainChannelInfo& GetChannel(u32 channel) const;

        /// @brief One channel's pool array image.
        [[nodiscard]] const Ref<Image>& GetChannelImage(u32 channel) const;

        /// @brief A sampled view over every layer of one channel's pool array.
        ///
        /// Every layer is kept in a sampled layout outside a tile's own tick, so the view is bindable
        /// by any pass after the frame's generation; only resident layers hold meaningful texels.
        [[nodiscard]] const Ref<ImageView>& GetChannelArrayView(u32 channel) const;

        /// @brief Changes the pool's capacity in place.
        ///
        /// A grow reallocates the arrays and copies the resident layers; a shrink evicts by the
        /// residency rules first and compacts what remains. The pinned top survives either, and a
        /// shrink below what cannot be evicted stops there. Tiles still generating are cancelled
        /// and requested again by whoever wants them.
        /// @param capacity The new capacity in tiles; clamped to the device's array-layer limit.
        void SetCapacity(u32 capacity);

        /// @brief Whether a tile is resident.
        [[nodiscard]] bool IsResident(CubeSphereTileKey key) const;

        /// @brief Whether a tile is requested and still generating.
        [[nodiscard]] bool IsPending(CubeSphereTileKey key) const;

        /// @brief Whether every pinned tile is resident.
        [[nodiscard]] bool IsPinnedTopResident() const;

        /// @brief A resident tile and its measured bound, or nullopt while it is not resident.
        [[nodiscard]] optional<SphereTerrainResidentTile> FindResident(CubeSphereTileKey key) const;

        /// @brief The deepest resident tile covering a body-fixed direction, with its bound.
        ///
        /// For a consumer aligning its own work (a collider stream, a scatter) to what is drawn.
        /// @param direction Any non-zero body-fixed direction.
        [[nodiscard]] SphereTerrainResidentTile FindDeepestResident(const dvec3& direction) const;

        /// @brief Marks a tile wanted this frame, so it is not evicted (or, still generating, not
        ///        cancelled) while it is.
        void Want(CubeSphereTileKey key);

        /// @brief Requests a tile, or re-prioritises it while it is still generating.
        ///
        /// Marks it wanted. A tile is requested only when its parent is resident.
        /// @param key       The tile.
        /// @param priority  The generated-texture priority; higher runs first.
        /// @return True when the tile is resident or now generating; false when it waits.
        bool Request(CubeSphereTileKey key, i32 priority);

        /// @brief What the pool holds.
        [[nodiscard]] SphereTerrainSourceStats GetStats() const;

        /// @brief The residency core, which a renderer's selection reads.
        [[nodiscard]] const SphereTerrainResidency& GetResidency() const;

        /// @brief The generated-texture priority of a pinned tile.
        static constexpr i32 PinnedPriority = 900000;

        /// @brief The generated-texture priority of a restored tile's measurement.
        static constexpr i32 MeasurePriority = 950000;

    private:
        /// @brief The source's implementation, defined in its translation unit.
        struct Impl;

        explicit SphereTerrainSource(Unique<Impl> impl);

        /// @brief The implementation.
        Unique<Impl> m_Impl;
    };
}
