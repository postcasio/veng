#pragma once

#include <array>

#include <Veng/Veng.h>

namespace Veng
{
    /// @brief The cube-sphere: a sphere's surface as six square faces, each a quadtree of tiles.
    ///
    /// Each face is the square `u, v ∈ [−1, 1]` with a fixed right-handed basis (Normal, U, V),
    /// `U × V = Normal`. A face coordinate maps to the cube plane through the **equal-angle warp**
    /// `x = tan(π/4 · u)`, and the cube point `Normal + x·U + y·V` projects to the sphere along its
    /// own direction. The warp holds the largest-to-smallest tile area ratio on a face near 1.4,
    /// against about 5.2 for the plain gnomonic cube, and every tile is a rectangle in `u, v`.
    ///
    /// A straight line on the cube plane projects to a great circle, so every tile edge (a line of
    /// constant `x` or `y`) is a great-circle arc. Directions are the sphere's own body-fixed frame.
    ///
    /// Device-free and double precision throughout, except the `F32` twins, which are what the
    /// shaders evaluate (`Veng/sphere_terrain_mapping.slang`) and are kept bit-for-bit in step with
    /// them so a unit case can bound the device's arithmetic on the host.
    namespace CubeSphere
    {
        /// @brief The number of cube faces.
        inline constexpr u32 FaceCount = 6;

        /// @brief The deepest quadtree level a tile key can name.
        ///
        /// A level-24 tile on a sphere of 6,400 km radius is about 60 cm across.
        inline constexpr u32 MaxLevel = 24;

        /// @brief The six faces, in key order.
        enum class Face : u8
        {
            /// @brief The face whose normal is +X.
            PosX = 0,
            /// @brief The face whose normal is −X.
            NegX = 1,
            /// @brief The face whose normal is +Y.
            PosY = 2,
            /// @brief The face whose normal is −Y.
            NegY = 3,
            /// @brief The face whose normal is +Z.
            PosZ = 4,
            /// @brief The face whose normal is −Z.
            NegZ = 5,
        };

        /// @brief A tile's four edges.
        enum class Edge : u8
        {
            /// @brief The edge at the tile's lowest `u`.
            NegU = 0,
            /// @brief The edge at the tile's highest `u`.
            PosU = 1,
            /// @brief The edge at the tile's lowest `v`.
            NegV = 2,
            /// @brief The edge at the tile's highest `v`.
            PosV = 3,
        };

        /// @brief A face's right-handed basis: its outward normal and its two tangent axes.
        struct FaceBasis
        {
            /// @brief The face's outward normal, a coordinate axis.
            dvec3 Normal{0.0};
            /// @brief The axis `u` increases along.
            dvec3 U{0.0};
            /// @brief The axis `v` increases along; `U × V = Normal`.
            dvec3 V{0.0};
        };

        /// @brief A point on the cube-sphere, as a face and its face coordinates.
        struct FacePoint
        {
            /// @brief The face the point lies on.
            Face OnFace = Face::PosX;
            /// @brief The `u` face coordinate, in [−1, 1].
            f64 U = 0.0;
            /// @brief The `v` face coordinate, in [−1, 1].
            f64 V = 0.0;
        };

        /// @brief Returns a face's basis.
        /// @param face The face.
        [[nodiscard]] VE_API const FaceBasis& GetFaceBasis(Face face);

        /// @brief Maps a face coordinate to the cube plane: `tan(π/4 · u)`.
        [[nodiscard]] VE_API f64 WarpToCube(f64 u);

        /// @brief Maps a cube-plane coordinate back to its face coordinate: `atan(x) · 4/π`.
        [[nodiscard]] VE_API f64 CubeToWarp(f64 x);

        /// @brief The unit direction of a face coordinate.
        /// @param face The face.
        /// @param u    The `u` face coordinate.
        /// @param v    The `v` face coordinate.
        [[nodiscard]] VE_API dvec3 FaceToDirection(Face face, f64 u, f64 v);

        /// @brief The face and face coordinates of a direction.
        ///
        /// The face is the one whose normal the direction's largest component points along; a
        /// direction exactly on a face edge resolves to the lower-numbered face.
        /// @param direction Any non-zero direction; it need not be unit length.
        [[nodiscard]] VE_API FacePoint DirectionToFace(const dvec3& direction);

        /// @brief The face-coordinate fraction across a tile at tile parameter @p s, in double.
        ///
        /// A tile spans the warp angles `θ0 … θ0 + Δθ` along one axis (`θ = π/4 · u`), and the
        /// fraction of its cube-plane span at parameter `s` is
        /// `(tan(θ0 + sΔθ) − tan θ0) / (tan(θ0 + Δθ) − tan θ0)`. It is evaluated through
        /// `tan a − tan b = sin(a − b) / (cos a · cos b)` as
        /// `sin(sΔθ)·cos(θ0 + Δθ) / (sin Δθ · cos(θ0 + sΔθ))`, which never subtracts two nearly
        /// equal tangents, so it keeps full relative precision at any depth.
        /// @param s       The tile parameter; 0 and 1 are the tile's edges.
        /// @param theta0  The tile's starting warp angle.
        /// @param dtheta  The tile's warp-angle span.
        [[nodiscard]] VE_API f64 TileFraction(f64 s, f64 theta0, f64 dtheta);

        /// @brief The f32 twin of TileFraction that the shaders evaluate.
        ///
        /// The same quotient, divided through by `cos θ0` so that it takes the tile's cube-plane
        /// start `x0 = tan θ0` and needs only the sine and cosine of `sΔθ` and `Δθ`:
        /// `S(sΔθ)·(C(Δθ) − x0·S(Δθ)) / (S(Δθ)·(C(sΔθ) − x0·S(sΔθ)))`. `S` and `C` are fixed
        /// polynomials accurate to f32 on |x| ≤ 1.7 (CubeSphereSinF32, CubeSphereCosF32), so the
        /// result does not depend on a device's transcendental precision or its fast-math mode. It
        /// is exactly 0 at s = 0, exactly 1 at s = 1, and strictly increasing.
        /// @param s       The tile parameter.
        /// @param x0      The tile's cube-plane start, `tan θ0`.
        /// @param dtheta  The tile's warp-angle span.
        [[nodiscard]] VE_API f32 TileFractionF32(f32 s, f32 x0, f32 dtheta);

        /// @brief The polynomial sine TileFractionF32 evaluates, accurate to f32 on |x| ≤ 1.7.
        [[nodiscard]] VE_API f32 SinF32(f32 x);

        /// @brief The polynomial cosine TileFractionF32 evaluates, accurate to f32 on |x| ≤ 1.7.
        [[nodiscard]] VE_API f32 CosF32(f32 x);
    }

    /// @brief A cube-sphere quadtree tile, packed into 64 bits.
    ///
    /// Bits 0–23 hold `x`, 24–47 `y`, 48–52 the level and 53–55 the face. Level 0 is a whole face;
    /// a level-`L` tile is one of `2^L × 2^L` on its face, `x` counting along `u` and `y` along `v`.
    /// The navigation functions are pure, and a neighbour across a face edge maps its coordinates
    /// through that edge's rotation.
    struct VE_API CubeSphereTileKey
    {
        /// @brief The key that names no tile.
        static constexpr u64 InvalidValue = ~0ull;

        /// @brief The packed key.
        u64 Value = InvalidValue;

        /// @brief Packs a tile key.
        /// @param face   The face.
        /// @param level  The level, at most CubeSphere::MaxLevel.
        /// @param x      The tile's column, below `2^level`.
        /// @param y      The tile's row, below `2^level`.
        [[nodiscard]] static constexpr CubeSphereTileKey
        Make(const CubeSphere::Face face, const u32 level, const u32 x, const u32 y)
        {
            return {.Value = (static_cast<u64>(face) << 53) | (static_cast<u64>(level) << 48) |
                             (static_cast<u64>(y) << 24) | static_cast<u64>(x)};
        }

        /// @brief Wraps a packed value.
        [[nodiscard]] static constexpr CubeSphereTileKey FromValue(const u64 value)
        {
            return {.Value = value};
        }

        /// @brief Whether the key names a tile.
        [[nodiscard]] constexpr bool IsValid() const { return Value != InvalidValue; }

        /// @brief The tile's face.
        [[nodiscard]] constexpr CubeSphere::Face GetFace() const
        {
            return static_cast<CubeSphere::Face>((Value >> 53) & 0x7u);
        }

        /// @brief The tile's level.
        [[nodiscard]] constexpr u32 GetLevel() const
        {
            return static_cast<u32>((Value >> 48) & 0x1Fu);
        }

        /// @brief The tile's column along `u`.
        [[nodiscard]] constexpr u32 GetX() const { return static_cast<u32>(Value & 0xFFFFFFu); }

        /// @brief The tile's row along `v`.
        [[nodiscard]] constexpr u32 GetY() const
        {
            return static_cast<u32>((Value >> 24) & 0xFFFFFFu);
        }

        /// @brief The tile containing this one at the level above.
        /// @pre The level is above zero.
        [[nodiscard]] CubeSphereTileKey GetParent() const;

        /// @brief One of the four tiles this one splits into.
        /// @param index Bit 0 selects the higher-`u` half, bit 1 the higher-`v` half.
        /// @pre The level is below CubeSphere::MaxLevel.
        [[nodiscard]] CubeSphereTileKey GetChild(u32 index) const;

        /// @brief Which of its parent's children this tile is, in GetChild's numbering.
        [[nodiscard]] constexpr u32 GetQuadrant() const
        {
            return (GetX() & 1u) | ((GetY() & 1u) << 1);
        }

        /// @brief The same-level tile across one edge, on this face or the next.
        /// @param edge The edge crossed.
        [[nodiscard]] CubeSphereTileKey GetNeighbour(CubeSphere::Edge edge) const;

        /// @brief Whether this tile is an ancestor of (strictly contains) another.
        [[nodiscard]] bool IsAncestorOf(CubeSphereTileKey other) const;

        /// @brief Whether two keys name the same tile.
        [[nodiscard]] constexpr bool operator==(const CubeSphereTileKey&) const = default;
    };

    /// @brief A tile's placement on the unit sphere, in double.
    ///
    /// Corners are indexed `i = sx + 2·sy`, `sx` and `sy` 0 at the tile's low `u`/`v` edge.
    struct CubeSphereTileGeometry
    {
        /// @brief The four corner unit directions.
        std::array<dvec3, 4> Corners{};
        /// @brief The four corners' cube-point norms, `|Q_i| = sqrt(1 + x² + y²)`.
        std::array<f64, 4> CornerNorms{};
        /// @brief The unit direction at the tile's parametric centre (`s = t = ½`).
        dvec3 Center{0.0};
        /// @brief The tile's starting warp angle along `u`, `π/4 · u0`.
        f64 ThetaStartU = 0.0;
        /// @brief The tile's starting warp angle along `v`, `π/4 · v0`.
        f64 ThetaStartV = 0.0;
        /// @brief The tile's warp-angle span along each axis, `π/4 · Δu`.
        f64 ThetaSpan = 0.0;
        /// @brief The tile's cube-plane start along `u`, `tan θ0u`.
        f64 CubeStartU = 0.0;
        /// @brief The tile's cube-plane start along `v`, `tan θ0v`.
        f64 CubeStartV = 0.0;
    };

    /// @brief The placement of a tile on the unit sphere.
    [[nodiscard]] VE_API CubeSphereTileGeometry GetCubeSphereTileGeometry(CubeSphereTileKey key);

    /// @brief The unit direction at tile parameters (s, t), in double.
    ///
    /// The reference the f32 forms are measured against: the cube point bilinear in the corners'
    /// cube points at the warped fractions, normalised.
    /// @param geometry The tile's placement.
    /// @param s        The parameter along `u`; 0 and 1 are the tile's edges.
    /// @param t        The parameter along `v`.
    [[nodiscard]] VE_API dvec3 CubeSphereTileDirection(const CubeSphereTileGeometry& geometry,
                                                       f64 s, f64 t);

    /// @brief The f32 inputs of the precise texel-direction form, packed from a tile's geometry.
    ///
    /// The shaders' `SphereTerrainTileConstants` carries the same numbers.
    struct CubeSphereTileOffsetInputs
    {
        /// @brief Each corner's direction minus the centre's, computed in double, rounded to f32.
        std::array<vec3, 4> CornerOffsets{};
        /// @brief The corners' cube-point norms.
        vec4 CornerNorms{1.0f};
        /// @brief The tile's cube-plane starts and warp span: (x0u, Δθ, x0v, Δθ).
        vec4 Warp{0.0f};
    };

    /// @brief Packs the f32 inputs of the precise texel-direction form.
    [[nodiscard]] VE_API CubeSphereTileOffsetInputs
    GetCubeSphereTileOffsetInputs(const CubeSphereTileGeometry& geometry);

    /// @brief A direction relative to the tile's centre, precise at any depth: the f32 twin of the
    ///        shaders' `SphereTerrainTileOffset`.
    ///
    /// With bilinear weights `β_i` of the warped fractions, `ŵ_i = β_i·|Q_i| / Σ β_j·|Q_j|` and
    /// `δ = Σ ŵ_i (d_i − d_c)`, the chord point is `m = d_c + δ` and the offset of its direction is
    /// `n − d_c = (δ − d_c · (2 d_c·δ + |δ|²) / (1 + |m|)) / |m|`. Every term is built from the
    /// small differences `d_i − d_c`, so the offset is accurate to the f32 rounding of the tile's own
    /// size; the centre, which only double can hold to the metre on a large sphere, is added by
    /// whoever has it in double.
    /// @param inputs  The tile's packed inputs.
    /// @param center  The tile's centre direction, rounded to f32 (it enters only small terms).
    /// @param s       The parameter along `u`.
    /// @param t       The parameter along `v`.
    [[nodiscard]] VE_API vec3 CubeSphereTileOffsetF32(const CubeSphereTileOffsetInputs& inputs,
                                                      const vec3& center, f32 s, f32 t);
}
