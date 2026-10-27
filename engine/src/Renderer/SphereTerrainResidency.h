#pragma once

#include <unordered_map>

#include <Veng/Veng.h>
#include <Veng/Math/CubeSphere.h>

namespace Veng::Renderer
{
    /// @brief Where a tile the pool holds stands.
    enum class SphereTerrainTileState : u8
    {
        /// @brief Requested; its layer is reserved and its generation has not completed.
        Pending,
        /// @brief Generated; its layer holds its texels.
        Resident,
    };

    /// @brief A tile's height bound as the residency core holds it.
    struct SphereTerrainTileBound
    {
        /// @brief Whether the bound was measured; until then it is meaningless.
        bool Measured = false;
        /// @brief The lowest height over the tile's interior, in metres.
        f32 Min = 0.0f;
        /// @brief The highest height over the tile's interior, in metres.
        f32 Max = 0.0f;
        /// @brief The tile's largest departure from its parent's grid, in metres.
        f32 MorphError = 0.0f;
    };

    /// @brief One tile the pool holds.
    struct SphereTerrainTileRecord
    {
        /// @brief The tile.
        CubeSphereTileKey Key;
        /// @brief Its pool layer.
        u32 Layer = 0;
        /// @brief Pending or resident.
        SphereTerrainTileState State = SphereTerrainTileState::Pending;
        /// @brief Whether it belongs to the pinned top, which is never evicted.
        bool Pinned = false;
        /// @brief How many of its children the pool holds, pending or resident.
        u8 Children = 0;
        /// @brief Distinguishes this holding of the key from an earlier one, so a late answer for an
        ///        evicted tile is not applied to its re-request.
        u32 Generation = 0;
        /// @brief The last frame a renderer wanted it.
        u64 LastWanted = 0;
        /// @brief Its measured bound.
        SphereTerrainTileBound Bound;
    };

    /// @brief What the pool holds, as a selection reads it.
    class SphereTerrainTileOracle
    {
    public:
        /// @brief Virtual for subclassing.
        virtual ~SphereTerrainTileOracle() = default;

        /// @brief The pool's record of a tile, or null when it holds none.
        [[nodiscard]] virtual const SphereTerrainTileRecord* Find(CubeSphereTileKey key) const = 0;
    };

    /// @brief The device-free residency core of a sphere terrain's tile pool.
    ///
    /// Holds which tiles occupy which layers and enforces the rules: requests only under a
    /// resident parent (so the held set is closed under parent), eviction of the least recently
    /// wanted tile that is not pinned, has no held child and was wanted neither this frame nor the
    /// last, and never more tiles than layers. It knows nothing of images or jobs, so the policy is
    /// unit-tested against an instant generator.
    class VE_API SphereTerrainResidency final : public SphereTerrainTileOracle
    {
    public:
        /// @brief The outcome of a request.
        enum class RequestOutcome : u8
        {
            /// @brief The tile now holds a layer and is pending.
            Accepted,
            /// @brief The pool already holds the tile.
            AlreadyHeld,
            /// @brief The tile's parent is not resident.
            ParentMissing,
            /// @brief The pool is full and nothing is evictable; the request waits.
            Deferred,
        };

        /// @brief A request's outcome, its layer, and the tile it evicted.
        struct RequestResult
        {
            /// @brief What happened.
            RequestOutcome Outcome = RequestOutcome::Deferred;
            /// @brief The tile's layer when Accepted or AlreadyHeld.
            u32 Layer = 0;
            /// @brief The tile's generation when Accepted or AlreadyHeld.
            u32 Generation = 0;
            /// @brief The tile evicted to make room, or an invalid key.
            CubeSphereTileKey Evicted;
        };

        /// @brief A capacity change's layer moves and the tiles it evicted.
        struct CapacityChange
        {
            /// @brief The capacity reached; above the request when the held tiles would not fit.
            u32 Capacity = 0;
            /// @brief Each kept tile whose layer changed, as (old layer, new layer).
            vector<std::pair<u32, u32>> Moves;
            /// @brief The tiles evicted to fit.
            vector<CubeSphereTileKey> Evicted;
        };

        /// @brief Creates an empty pool.
        /// @param capacity     The number of layers.
        /// @param pinnedLevel  The deepest level of the pinned top.
        SphereTerrainResidency(u32 capacity, u32 pinnedLevel);

        /// @brief Advances the frame wants are stamped with.
        void BeginFrame(u64 frame);

        /// @brief The frame wants are stamped with.
        [[nodiscard]] u64 GetFrame() const { return m_Frame; }

        /// @copydoc SphereTerrainTileOracle::Find
        [[nodiscard]] const SphereTerrainTileRecord* Find(CubeSphereTileKey key) const override;

        /// @brief Whether a tile is resident.
        [[nodiscard]] bool IsResident(CubeSphereTileKey key) const;

        /// @brief Marks a held tile wanted this frame; a tile not held is ignored.
        void MarkWanted(CubeSphereTileKey key);

        /// @brief Reserves a layer for a tile, evicting the least recently wanted evictable tile
        ///        when the pool is full.
        [[nodiscard]] RequestResult Request(CubeSphereTileKey key);

        /// @brief Marks a pending tile resident.
        /// @return False when the tile is not held pending at that generation.
        bool Complete(CubeSphereTileKey key, u32 generation);

        /// @brief Records a resident tile's measured bound.
        /// @return False when the tile is not held at that generation.
        bool SetBound(CubeSphereTileKey key, u32 generation, const SphereTerrainTileBound& bound);

        /// @brief Drops a held tile and frees its layer.
        /// @pre The tile holds no child.
        /// @return False when the tile is not held.
        bool Remove(CubeSphereTileKey key);

        /// @brief The pending tiles wanted in neither this frame nor the last, which a caller
        ///        cancels; pinned tiles are never listed.
        [[nodiscard]] vector<CubeSphereTileKey> CollectStalePending() const;

        /// @brief Changes the number of layers: evicts to fit, then compacts every tile into the
        ///        new range.
        /// @pre No tile is pending.
        [[nodiscard]] CapacityChange SetCapacity(u32 capacity);

        /// @brief The number of layers.
        [[nodiscard]] u32 GetCapacity() const { return m_Capacity; }

        /// @brief The tiles held, pending or resident.
        [[nodiscard]] u32 GetHeldCount() const { return static_cast<u32>(m_Tiles.size()); }

        /// @brief The resident tiles.
        [[nodiscard]] u32 GetResidentCount() const;

        /// @brief The pending tiles.
        [[nodiscard]] u32 GetPendingCount() const;

        /// @brief The resident pinned tiles.
        [[nodiscard]] u32 GetPinnedResidentCount() const;

        /// @brief The deepest pinned level.
        [[nodiscard]] u32 GetPinnedLevel() const { return m_PinnedLevel; }

        /// @brief Tiles evicted over the core's lifetime.
        [[nodiscard]] u64 GetEvictions() const { return m_Evictions; }

        /// @brief Requests that waited on a full pool over the core's lifetime.
        [[nodiscard]] u64 GetDeferred() const { return m_Deferred; }

        /// @brief Every held tile, in no particular order.
        [[nodiscard]] const std::unordered_map<u64, SphereTerrainTileRecord>& GetTiles() const
        {
            return m_Tiles;
        }

    private:
        /// @brief The evictable tile wanted least recently, or an invalid key.
        [[nodiscard]] CubeSphereTileKey FindEvictable() const;

        /// @brief Drops a tile and returns its layer to the free list.
        void Drop(CubeSphereTileKey key);

        /// @brief The number of layers.
        u32 m_Capacity = 0;
        /// @brief The deepest pinned level.
        u32 m_PinnedLevel = 0;
        /// @brief The frame wants are stamped with.
        u64 m_Frame = 0;
        /// @brief The source of each holding's generation.
        u32 m_NextGeneration = 1;
        /// @brief Tiles evicted.
        u64 m_Evictions = 0;
        /// @brief Requests deferred.
        u64 m_Deferred = 0;
        /// @brief Every held tile by key.
        std::unordered_map<u64, SphereTerrainTileRecord> m_Tiles;
        /// @brief The free layers, taken from the back.
        vector<u32> m_FreeLayers;
    };
}
