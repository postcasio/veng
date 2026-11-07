#pragma once

#include <array>

#include <Veng/Veng.h>
#include <Veng/Math/CubeSphere.h>
#include <Veng/Renderer/SphereTerrain.h>

namespace Veng::Renderer
{
    /// @brief A tile's bound: a box in the tile's own frame (its centre's tangent plane and
    ///        normal), over its corners, edge midpoints and centre at both ends of its working
    ///        height range, grown by the sphere's sagitta between those samples.
    ///
    /// Oriented to the tile, so its diameter is the tile's own whatever face the tile is on.
    struct SphereTerrainTileBox
    {
        /// @brief The frame's origin, on the sphere below the tile's centre, body-fixed metres.
        dvec3 Origin{0.0};
        /// @brief The frame's axes: across the tile along u, across it along v, and outward.
        std::array<dvec3, 3> Axes{};
        /// @brief The box's lower corner in the frame.
        dvec3 Min{0.0};
        /// @brief The box's upper corner in the frame.
        dvec3 Max{0.0};
    };

    /// @brief The height-free half of a tile's bound box: its frame and the unit directions it
    ///        samples, from which ComputeSphereTerrainTileBox builds the box over any height range.
    struct SphereTerrainTileFrame
    {
        /// @brief The frame's axes: across the tile along u, across it along v, and outward.
        std::array<dvec3, 3> Axes{};
        /// @brief The sampled unit directions: the corners, the edge midpoints and the centre.
        std::array<dvec3, 9> Samples{};
        /// @brief `2 sin²(g)` for the half-gap `g` between samples; times a radius, the sagitta
        ///        the box grows by.
        f64 SagittaFactor = 0.0;
    };

    /// @brief What a tile's drawing reads that depends on its key alone, computed once while the
    ///        pool holds it.
    struct SphereTerrainTileFacts
    {
        /// @brief The tile's placement on the unit sphere.
        CubeSphereTileGeometry Geometry;
        /// @brief The height-free half of its bound box.
        SphereTerrainTileFrame Frame;
        /// @brief Its packed constants (PackSphereTerrainTileConstants).
        SphereTerrainTileConstants Constants;
        /// @brief The arc terms of its fraction along u and v: (S(Δθ), C(Δθ) − x0u·S(Δθ),
        ///        S(Δθ), C(Δθ) − x0v·S(Δθ)) (CubeSphere::TileArcF32).
        vec4 Arc{0.0f};
        /// @brief The squared chords |d0−d1|², |d0−d2|², |d0−d3|², |d1−d2|².
        vec4 ChordsA{0.0f};
        /// @brief The squared chords |d1−d3|², |d2−d3|².
        vec2 ChordsB{0.0f};
        /// @brief The tile's unit tangent along u, body-fixed.
        dvec3 TangentU{0.0};
        /// @brief The tile's edge length along its low v edge, in metres.
        f64 Edge = 0.0;
        /// @brief The generator's words for the tile (SphereTerrainGenerator::WriteTileDrawData).
        std::array<u32, MaxSphereTerrainTileWords> Words{};
    };

    /// @brief Computes a tile's facts on a sphere of a radius; the words are left zero.
    [[nodiscard]] VE_API SphereTerrainTileFacts MakeSphereTerrainTileFacts(CubeSphereTileKey key,
                                                                           f64 radius);

    /// @brief The height-free half of a tile's bound box.
    [[nodiscard]] VE_API SphereTerrainTileFrame
    MakeSphereTerrainTileFrame(const CubeSphereTileGeometry& geometry);

    /// @brief Builds a tile's bound box over a height range from its frame.
    [[nodiscard]] VE_API SphereTerrainTileBox ComputeSphereTerrainTileBox(
        const SphereTerrainTileFrame& frame, f64 radius, f32 minHeight, f32 maxHeight);
}
