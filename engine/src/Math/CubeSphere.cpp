#include <Veng/Math/CubeSphere.h>

#include <cmath>
#include <numbers>

#include <Veng/Assert.h>

namespace Veng
{
    namespace
    {
        constexpr f64 QuarterPi = std::numbers::pi / 4.0;

        // Each face's (Normal, U, V), right-handed: U x V = Normal.
        const std::array<CubeSphere::FaceBasis, CubeSphere::FaceCount> s_Bases = {{
            {.Normal = {1, 0, 0}, .U = {0, 0, -1}, .V = {0, 1, 0}},
            {.Normal = {-1, 0, 0}, .U = {0, 0, 1}, .V = {0, 1, 0}},
            {.Normal = {0, 1, 0}, .U = {1, 0, 0}, .V = {0, 0, -1}},
            {.Normal = {0, -1, 0}, .U = {1, 0, 0}, .V = {0, 0, 1}},
            {.Normal = {0, 0, 1}, .U = {1, 0, 0}, .V = {0, 1, 0}},
            {.Normal = {0, 0, -1}, .U = {-1, 0, 0}, .V = {0, 1, 0}},
        }};

        // An integer axis vector, so the cross-face fold is exact.
        struct IntVec
        {
            i64 X = 0;
            i64 Y = 0;
            i64 Z = 0;
        };

        IntVec ToInt(const dvec3& v)
        {
            return {
                .X = static_cast<i64>(v.x), .Y = static_cast<i64>(v.y), .Z = static_cast<i64>(v.z)};
        }

        i64 Dot(const IntVec& a, const IntVec& b)
        {
            return (a.X * b.X) + (a.Y * b.Y) + (a.Z * b.Z);
        }

        IntVec Scale(const IntVec& v, const i64 s)
        {
            return {.X = v.X * s, .Y = v.Y * s, .Z = v.Z * s};
        }

        IntVec Add(const IntVec& a, const IntVec& b)
        {
            return {.X = a.X + b.X, .Y = a.Y + b.Y, .Z = a.Z + b.Z};
        }

        CubeSphere::Face FaceOfNormal(const IntVec& normal)
        {
            for (u32 f = 0; f < CubeSphere::FaceCount; f++)
            {
                const IntVec n = ToInt(s_Bases[f].Normal);
                if (n.X == normal.X && n.Y == normal.Y && n.Z == normal.Z)
                {
                    return static_cast<CubeSphere::Face>(f);
                }
            }
            VE_ASSERT(false, "cube-sphere fold produced a non-axis normal");
        }

        // Shared by the numerator and denominator of TileFractionF32, so s = 1 reproduces the same
        // rounding on both sides and the quotient is exactly one.
        f32 Shifted(const f32 c, const f32 s, const f32 x0)
        {
            return c - (x0 * s);
        }
    }

    namespace CubeSphere
    {
        const FaceBasis& GetFaceBasis(const Face face)
        {
            return s_Bases[static_cast<u32>(face)];
        }

        f64 WarpToCube(const f64 u)
        {
            return std::tan(QuarterPi * u);
        }

        f64 CubeToWarp(const f64 x)
        {
            return std::atan(x) / QuarterPi;
        }

        dvec3 FaceToDirection(const Face face, const f64 u, const f64 v)
        {
            const FaceBasis& basis = GetFaceBasis(face);
            return glm::normalize(basis.Normal + (WarpToCube(u) * basis.U) +
                                  (WarpToCube(v) * basis.V));
        }

        FacePoint DirectionToFace(const dvec3& direction)
        {
            const dvec3 a = glm::abs(direction);
            u32 face = 0;
            if (a.x >= a.y && a.x >= a.z)
            {
                face = direction.x >= 0.0 ? 0u : 1u;
            }
            else if (a.y >= a.z)
            {
                face = direction.y >= 0.0 ? 2u : 3u;
            }
            else
            {
                face = direction.z >= 0.0 ? 4u : 5u;
            }
            const FaceBasis& basis = s_Bases[face];
            const f64 n = glm::dot(direction, basis.Normal);
            return {
                .OnFace = static_cast<Face>(face),
                .U = CubeToWarp(glm::dot(direction, basis.U) / n),
                .V = CubeToWarp(glm::dot(direction, basis.V) / n),
            };
        }

        f64 TileFraction(const f64 s, const f64 theta0, const f64 dtheta)
        {
            return (std::sin(s * dtheta) * std::cos(theta0 + dtheta)) /
                   (std::sin(dtheta) * std::cos(theta0 + (s * dtheta)));
        }

        f32 SinF32(const f32 x)
        {
            // Taylor to x^13: the truncation is below 1e-9 on |x| <= 1.7.
            const f32 x2 = x * x;
            f32 p = 1.0f / 6227020800.0f;
            p = (p * x2) - (1.0f / 39916800.0f);
            p = (p * x2) + (1.0f / 362880.0f);
            p = (p * x2) - (1.0f / 5040.0f);
            p = (p * x2) + (1.0f / 120.0f);
            p = (p * x2) - (1.0f / 6.0f);
            return x + (x * x2 * p);
        }

        f32 CosF32(const f32 x)
        {
            // Taylor to x^14: the truncation is below 1e-10 on |x| <= 1.7.
            const f32 x2 = x * x;
            f32 p = -1.0f / 87178291200.0f;
            p = (p * x2) + (1.0f / 479001600.0f);
            p = (p * x2) - (1.0f / 3628800.0f);
            p = (p * x2) + (1.0f / 40320.0f);
            p = (p * x2) - (1.0f / 720.0f);
            p = (p * x2) + (1.0f / 24.0f);
            p = (p * x2) - 0.5f;
            return 1.0f + (x2 * p);
        }

        f32 TileFractionF32(const f32 s, const f32 x0, const f32 dtheta)
        {
            const f32 a = s * dtheta;
            const f32 sinA = SinF32(a);
            const f32 sinD = SinF32(dtheta);
            return (sinA * Shifted(CosF32(dtheta), sinD, x0)) /
                   (sinD * Shifted(CosF32(a), sinA, x0));
        }
    }

    CubeSphereTileKey CubeSphereTileKey::GetParent() const
    {
        VE_ASSERT(GetLevel() > 0, "a level-0 tile has no parent");
        return Make(GetFace(), GetLevel() - 1, GetX() >> 1, GetY() >> 1);
    }

    CubeSphereTileKey CubeSphereTileKey::GetChild(const u32 index) const
    {
        VE_ASSERT(GetLevel() < CubeSphere::MaxLevel, "a level-{} tile has no children",
                  CubeSphere::MaxLevel);
        return Make(GetFace(), GetLevel() + 1, (GetX() << 1) | (index & 1u),
                    (GetY() << 1) | ((index >> 1) & 1u));
    }

    CubeSphereTileKey CubeSphereTileKey::GetNeighbour(const CubeSphere::Edge edge) const
    {
        // Tile centres in doubled integer face coordinates: odd values in (-n, n). A step past the
        // face edge is folded over it exactly, through the integer face bases.
        const u32 level = GetLevel();
        const i64 n = i64{1} << level;
        i64 cu = (2 * static_cast<i64>(GetX())) + 1 - n;
        i64 cv = (2 * static_cast<i64>(GetY())) + 1 - n;
        switch (edge)
        {
        case CubeSphere::Edge::NegU:
            cu -= 2;
            break;
        case CubeSphere::Edge::PosU:
            cu += 2;
            break;
        case CubeSphere::Edge::NegV:
            cv -= 2;
            break;
        case CubeSphere::Edge::PosV:
            cv += 2;
            break;
        }

        CubeSphere::Face face = GetFace();
        if (std::abs(cu) > n || std::abs(cv) > n)
        {
            const CubeSphere::FaceBasis& basis = CubeSphere::GetFaceBasis(face);
            const IntVec normal = ToInt(basis.Normal);
            const IntVec axisU = ToInt(basis.U);
            const IntVec axisV = ToInt(basis.V);
            IntVec point;
            IntVec newNormal;
            if (std::abs(cu) > n)
            {
                const i64 sign = cu > 0 ? 1 : -1;
                const i64 over = std::abs(cu) - n;
                point = Add(Add(Scale(normal, n - over), Scale(axisU, sign * n)), Scale(axisV, cv));
                newNormal = Scale(axisU, sign);
            }
            else
            {
                const i64 sign = cv > 0 ? 1 : -1;
                const i64 over = std::abs(cv) - n;
                point = Add(Add(Scale(normal, n - over), Scale(axisV, sign * n)), Scale(axisU, cu));
                newNormal = Scale(axisV, sign);
            }
            face = FaceOfNormal(newNormal);
            const CubeSphere::FaceBasis& next = CubeSphere::GetFaceBasis(face);
            cu = Dot(point, ToInt(next.U));
            cv = Dot(point, ToInt(next.V));
        }
        return Make(face, level, static_cast<u32>((cu + n - 1) / 2),
                    static_cast<u32>((cv + n - 1) / 2));
    }

    bool CubeSphereTileKey::IsAncestorOf(const CubeSphereTileKey other) const
    {
        const u32 level = GetLevel();
        const u32 otherLevel = other.GetLevel();
        if (otherLevel <= level || other.GetFace() != GetFace())
        {
            return false;
        }
        const u32 shift = otherLevel - level;
        return (other.GetX() >> shift) == GetX() && (other.GetY() >> shift) == GetY();
    }

    CubeSphereTileGeometry GetCubeSphereTileGeometry(const CubeSphereTileKey key)
    {
        const CubeSphere::FaceBasis& basis = CubeSphere::GetFaceBasis(key.GetFace());
        const f64 span = 2.0 / static_cast<f64>(1u << key.GetLevel());
        const f64 u0 = -1.0 + (span * key.GetX());
        const f64 v0 = -1.0 + (span * key.GetY());

        CubeSphereTileGeometry geometry{
            .ThetaStartU = QuarterPi * u0,
            .ThetaStartV = QuarterPi * v0,
            .ThetaSpan = QuarterPi * span,
            .CubeStartU = CubeSphere::WarpToCube(u0),
            .CubeStartV = CubeSphere::WarpToCube(v0),
        };
        for (u32 i = 0; i < 4; i++)
        {
            const f64 x = CubeSphere::WarpToCube(u0 + (span * (i & 1u)));
            const f64 y = CubeSphere::WarpToCube(v0 + (span * (i >> 1)));
            const dvec3 cube = basis.Normal + (x * basis.U) + (y * basis.V);
            geometry.CornerNorms[i] = glm::length(cube);
            geometry.Corners[i] = cube / geometry.CornerNorms[i];
        }
        geometry.Center = CubeSphereTileDirection(geometry, 0.5, 0.5);
        return geometry;
    }

    dvec3 CubeSphereTileDirection(const CubeSphereTileGeometry& geometry, const f64 s, const f64 t)
    {
        const f64 au = CubeSphere::TileFraction(s, geometry.ThetaStartU, geometry.ThetaSpan);
        const f64 av = CubeSphere::TileFraction(t, geometry.ThetaStartV, geometry.ThetaSpan);
        const std::array<f64, 4> beta = {(1.0 - au) * (1.0 - av), au * (1.0 - av), (1.0 - au) * av,
                                         au * av};
        dvec3 cube{0.0};
        for (u32 i = 0; i < 4; i++)
        {
            cube += beta[i] * geometry.CornerNorms[i] * geometry.Corners[i];
        }
        return glm::normalize(cube);
    }

    CubeSphereTileOffsetInputs GetCubeSphereTileOffsetInputs(const CubeSphereTileGeometry& geometry)
    {
        CubeSphereTileOffsetInputs inputs{
            .CornerNorms = vec4(static_cast<f32>(geometry.CornerNorms[0]),
                                static_cast<f32>(geometry.CornerNorms[1]),
                                static_cast<f32>(geometry.CornerNorms[2]),
                                static_cast<f32>(geometry.CornerNorms[3])),
            .Warp =
                vec4(static_cast<f32>(geometry.CubeStartU), static_cast<f32>(geometry.ThetaSpan),
                     static_cast<f32>(geometry.CubeStartV), static_cast<f32>(geometry.ThetaSpan)),
        };
        for (u32 i = 0; i < 4; i++)
        {
            inputs.CornerOffsets[i] = vec3(geometry.Corners[i] - geometry.Center);
        }
        return inputs;
    }

    vec3 CubeSphereTileOffsetF32(const CubeSphereTileOffsetInputs& inputs, const vec3& center,
                                 const f32 s, const f32 t)
    {
        const f32 au = CubeSphere::TileFractionF32(s, inputs.Warp.x, inputs.Warp.y);
        const f32 av = CubeSphere::TileFractionF32(t, inputs.Warp.z, inputs.Warp.w);
        const vec4 beta =
            vec4((1.0f - au) * (1.0f - av), au * (1.0f - av), (1.0f - au) * av, au * av) *
            inputs.CornerNorms;
        const f32 total = beta.x + beta.y + beta.z + beta.w;
        const vec3 delta =
            ((beta.x * inputs.CornerOffsets[0]) + (beta.y * inputs.CornerOffsets[1]) +
             (beta.z * inputs.CornerOffsets[2]) + (beta.w * inputs.CornerOffsets[3])) /
            total;
        const f32 shortfall = (2.0f * glm::dot(center, delta)) + glm::dot(delta, delta);
        const f32 m = std::sqrt(1.0f + shortfall);
        return (delta - (center * (shortfall / (1.0f + m)))) / m;
    }
}
