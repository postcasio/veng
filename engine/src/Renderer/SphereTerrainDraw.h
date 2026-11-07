#pragma once

#include <array>
#include <span>
#include <unordered_map>

#include <Veng/Veng.h>
#include <Veng/Asset/Material.h>
#include <Veng/Renderer/SphereTerrain.h>

#include "SphereTerrainVertex.h"

namespace Veng::Renderer
{
    /// @brief The surfaces a terrain draws: its own, then each level layer.
    inline constexpr u32 SphereTerrainSurfaceCount = 1 + MaxSphereTerrainLevelLayers;

    /// @brief What a terrain makes of a surface's material.
    enum class SphereTerrainMaterialVerdict : u8
    {
        /// @brief Drawn into the g-buffer and the shadow views.
        Opaque,
        /// @brief Drawn in the translucent pass, casting nothing: a level layer of a Translucent
        ///        material.
        Translucent,
        /// @brief Not drawn: the material does not declare "sphereTerrain".
        NotTerrain,
        /// @brief Not drawn: the terrain's own surface asks for a Translucent material. The ground
        ///        is what every level layer is drawn over, so it is always opaque.
        TranslucentGround,
        /// @brief Not drawn: a domain no terrain surface draws in.
        WrongDomain,
    };

    /// @brief Decides whether, and in which pass, a terrain draws a surface with a material.
    ///
    /// Device-free and pure. A terrain's own surface draws with a Surface material declaring
    /// "sphereTerrain"; a level layer with a Surface or a Translucent one.
    /// @param domain        The parent material's domain.
    /// @param sphereTerrain Whether the parent material declares "sphereTerrain".
    /// @param levelLayer    Whether the surface is a level layer rather than the terrain's own.
    [[nodiscard]] VE_API SphereTerrainMaterialVerdict
    CheckSphereTerrainMaterial(MaterialDomain domain, bool sphereTerrain, bool levelLayer);

    /// @brief One view a terrain's tiles are culled against, in the sphere's body-fixed frame.
    struct SphereTerrainDrawView
    {
        /// @brief The view's planes about the eye: a point `q` (from the eye, body-fixed) is inside
        ///        when `dot(xyz, q) + w ≥ 0` for every plane. A plane of (0, 0, 0, 1) admits
        ///        everything.
        std::array<glm::dvec4, 6> Planes{};
        /// @brief Whether the view also culls tiles hidden below the horizon from the eye.
        bool Horizon = false;
    };

    /// @brief What one terrain's frame of records and per-view lists is built from.
    struct SphereTerrainFrameInput
    {
        /// @brief The tiles the selection drew.
        std::span<const SphereTerrainDrawnTile> Drawn;
        /// @brief The sphere's radius in metres.
        f64 Radius = 1.0;
        /// @brief The lowest height anywhere on the sphere.
        f32 MinHeight = 0.0f;
        /// @brief The highest height anywhere on the sphere.
        f32 MaxHeight = 0.0f;
        /// @brief The eye, body-fixed, in metres from the sphere's centre.
        dvec3 Eye{0.0};
        /// @brief The sphere's centre minus the eye, in the stable frame's (scene) axes.
        dvec3 CenterFromEye{0.0};
        /// @brief The rotation from the body-fixed frame into scene axes.
        glm::dquat Orientation{1.0, 0.0, 0.0, 0.0};
        /// @brief Last frame's centre in last frame's scene space, minus this frame's camera
        ///        position in this frame's scene space.
        dvec3 PrevCenterFromCamera{0.0};
        /// @brief Last frame's orientation.
        glm::dquat PrevOrientation{1.0, 0.0, 0.0, 0.0};
        /// @brief The uniform scale about the eye the terrain draws at.
        f32 Scale = 1.0f;
        /// @brief Whether the terrain casts into the shadow views; honoured only at scale one.
        bool CastsShadows = true;
        /// @brief The level layers' heights above the radius, in metres.
        std::array<f32, MaxSphereTerrainLevelLayers> LayerHeights{};
        /// @brief The level layers drawn.
        u32 LayerCount = 0;
        /// @brief Per level layer, whether it draws in the translucent pass: its main-view tiles go
        ///        to SphereTerrainFrame::TranslucentLists, and it casts into no shadow view.
        std::array<bool, MaxSphereTerrainLevelLayers> TranslucentLayers{};
        /// @brief Per surface, the drawn distance within which a main-view tile draws with the
        ///        surface's near material; zero draws none with it.
        ///
        /// A tile qualifies when its bound's farthest corner, times Scale, lies within it. Its
        /// g-buffer or translucent main-view entry then goes to the near lists instead.
        std::array<f32, SphereTerrainSurfaceCount> NearDistances{};
        /// @brief The main view.
        SphereTerrainDrawView Main;
        /// @brief The shadow views, in the frame's shadow-view order.
        std::span<const SphereTerrainDrawView> Shadows;
        /// @brief Per tile key, the arrival floor its morph takes this frame; null for none.
        const std::unordered_map<u64, f32>* ArrivalFloors = nullptr;
        /// @brief Whether every record carries SphereTerrainRecordTileOverlay.
        bool TileOverlay = false;
        /// @brief Fills a tile's generator words; null leaves them zero.
        function<void(CubeSphereTileKey, std::span<u32>)> TileData;
    };

    /// @brief One terrain's records and per-view lists for a frame.
    struct SphereTerrainFrame
    {
        /// @brief One record per tile some view draws.
        vector<SphereTerrainDrawRecord> Records;
        /// @brief Per view (0 the main view's g-buffer, 1 + n shadow view n), per surface (0 the
        ///        terrain, 1 + l level layer l), the records it draws, as indices into Records. A
        ///        translucent layer's lists here are empty.
        vector<std::array<vector<u32>, SphereTerrainSurfaceCount>> Lists;
        /// @brief Per surface, the records the main view draws in the translucent pass; empty but
        ///        for a translucent layer.
        std::array<vector<u32>, SphereTerrainSurfaceCount> TranslucentLists;
        /// @brief Per surface, the main view's g-buffer records drawn with the near material,
        ///        which Lists[0] then leaves out.
        std::array<vector<u32>, SphereTerrainSurfaceCount> NearLists;
        /// @brief Per surface, the main view's translucent records drawn with the near material,
        ///        which TranslucentLists then leaves out.
        std::array<vector<u32>, SphereTerrainSurfaceCount> NearTranslucentLists;
        /// @brief Drawn tiles the main view's frustum culled.
        u32 CulledByFrustum = 0;
        /// @brief Drawn tiles the main view culled below the horizon.
        u32 CulledByHorizon = 0;
    };

    /// @brief Culls a terrain's drawn tiles per view and builds the records its draws read.
    ///
    /// Device-free and pure. A tile is culled by a view when its bound (the selection's box over
    /// its working height range, widened to its level layers) lies wholly outside one of the
    /// view's planes, or — for a view culling by horizon — when all eight corners are hidden by
    /// the sphere of `Radius + MinHeight`. Culling is in unscaled metres, so the main view's lists
    /// do not depend on the scale; a terrain at a scale other than one, or not casting, draws into
    /// no shadow view. A level layer draws a tile only where the tile's working range dips below
    /// the layer's height. A translucent layer's main-view tiles go to the translucent lists alone,
    /// and it draws into no shadow view. A main-view tile whose bound lies wholly within a
    /// surface's near distance is listed in that surface's near list in place of its own; the
    /// shadow views take no near list.
    [[nodiscard]] VE_API SphereTerrainFrame
    BuildSphereTerrainFrame(const SphereTerrainFrameInput& input);
}
