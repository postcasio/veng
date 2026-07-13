#pragma once

#include <vector>

#include <Veng/Veng.h>

/// @brief Device-free generation of the Linearly-Transformed-Cosine (LTC) lookup tables.
///
/// Area lights (Rect/Sphere/Polygon) shade the GGX BRDF analytically by transforming a
/// clamped-cosine distribution through a per-(roughness, view-angle) matrix — the LTC
/// approximation of Heitz et al. This module fits that matrix for every table cell on the
/// CPU (a Nelder-Mead fit of the microfacet BRDF, pure glm — no device), producing the two
/// tables the lighting pass samples.
///
/// The fit depends only on the GGX BRDF — no scene, light, or environment input — so its result
/// is a fixed constant baked once into the core pack (the Raw asset `data/ltc_lut.bin`, matrix
/// table then magnitude table, RGBA32F). The runtime loads that asset; this generator is the
/// offline source of truth used to (re)produce the baked binary, not called at runtime.

namespace Veng::Renderer
{
    /// @brief The two fitted LTC lookup tables, laid out row-major for a Size×Size texture.
    ///
    /// Both are indexed by (roughness on the x axis, sqrt(1 - N·V) on the y axis). Matrix
    /// carries the inverse LTC transform (x,y,z,w = the four non-trivial Minv entries the
    /// lighting pass reconstructs); Magnitude carries the BRDF norm (x) and the Fresnel
    /// weight (y) the specular tint is split across.
    struct LtcLut
    {
        /// @brief Table edge length; the textures are Size×Size RGBA32F.
        static constexpr u32 Size = 64;

        /// @brief Row-major inverse-LTC-matrix entries (x = m00, y = m02, z = m20, w = m22).
        std::vector<vec4> Matrix;
        /// @brief Row-major BRDF magnitude (x) and Fresnel weight (y); zw unused.
        std::vector<vec4> Magnitude;
    };

    /// @brief Fits and returns the LTC lookup tables for the GGX BRDF (offline generator).
    ///
    /// Runs the full per-cell Nelder-Mead fit. Not called at runtime — it is the source of the
    /// baked `data/ltc_lut.bin` core-pack asset; rerun it to regenerate that binary if the fit
    /// changes (matrix table, then magnitude table, each written as RGBA32F).
    /// @return The Matrix and Magnitude tables, each LtcLut::Size² vec4 entries.
    [[nodiscard]] LtcLut GenerateLtcLut();
}
