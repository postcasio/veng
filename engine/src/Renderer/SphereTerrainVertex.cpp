#include "SphereTerrainVertex.h"

#include <algorithm>
#include <cmath>

namespace Veng::Renderer
{
    namespace
    {
        constexpr i32 Quads = static_cast<i32>(SphereTerrainGridQuads);
        constexpr u32 Row = SphereTerrainGridQuads + 1;

        // A skirt edge's vertex k, in CubeSphere::Edge order: NegU, PosU, NegV, PosV.
        ivec2 EdgeVertex(const u32 edge, const i32 k)
        {
            switch (edge)
            {
            case 0:
                return {0, k};
            case 1:
                return {Quads, k};
            case 2:
                return {k, 0};
            default:
                return {k, Quads};
            }
        }

        // The direction a skirt faces: away from its tile, across its edge.
        vec3 EdgeOutward(const u32 edge)
        {
            switch (edge)
            {
            case 0:
                return {-1.0f, 0.0f, 0.0f};
            case 1:
                return {1.0f, 0.0f, 0.0f};
            case 2:
                return {0.0f, -1.0f, 0.0f};
            default:
                return {0.0f, 1.0f, 0.0f};
            }
        }

        // A flat stand-in for the tile, with u, v and the outward vertical as x, y and z: the face
        // bases are right-handed, so a winding here is the winding on the sphere.
        vec3 Surrogate(const u32 index)
        {
            const SphereTerrainGridVertex vertex = DecodeSphereTerrainGridVertex(index);
            return {static_cast<f32>(vertex.Grid.x), static_cast<f32>(vertex.Grid.y),
                    vertex.Skirt ? -1.0f : 0.0f};
        }
    }

    SphereTerrainGridVertex DecodeSphereTerrainGridVertex(const u32 index)
    {
        if (index < SphereTerrainSurfaceVertices)
        {
            return {.Grid = {static_cast<i32>(index % Row), static_cast<i32>(index / Row)}};
        }
        const u32 skirt = index - SphereTerrainSurfaceVertices;
        return {.Grid = EdgeVertex(skirt / Row, static_cast<i32>(skirt % Row)), .Skirt = true};
    }

    std::array<u16, SphereTerrainGridIndexCount> BuildSphereTerrainGridIndices()
    {
        std::array<u16, SphereTerrainGridIndexCount> indices{};
        u32 cursor = 0;
        const auto Emit = [&](u32 a, u32 b, u32 c, const vec3& facing)
        {
            const vec3 normal =
                glm::cross(Surrogate(b) - Surrogate(a), Surrogate(c) - Surrogate(a));
            if (glm::dot(normal, facing) < 0.0f)
            {
                std::swap(b, c);
            }
            indices[cursor++] = static_cast<u16>(a);
            indices[cursor++] = static_cast<u16>(b);
            indices[cursor++] = static_cast<u16>(c);
        };

        const vec3 up(0.0f, 0.0f, 1.0f);
        for (u32 j = 0; j < SphereTerrainGridQuads; j++)
        {
            for (u32 i = 0; i < SphereTerrainGridQuads; i++)
            {
                const u32 v00 = (j * Row) + i;
                const u32 v10 = v00 + 1;
                const u32 v01 = v00 + Row;
                const u32 v11 = v01 + 1;
                Emit(v00, v10, v11, up);
                Emit(v00, v11, v01, up);
            }
        }
        for (u32 edge = 0; edge < 4; edge++)
        {
            const vec3 outward = EdgeOutward(edge);
            for (u32 k = 0; k < SphereTerrainGridQuads; k++)
            {
                const ivec2 a = EdgeVertex(edge, static_cast<i32>(k));
                const ivec2 b = EdgeVertex(edge, static_cast<i32>(k + 1));
                const u32 ea = (static_cast<u32>(a.y) * Row) + static_cast<u32>(a.x);
                const u32 eb = (static_cast<u32>(b.y) * Row) + static_cast<u32>(b.x);
                const u32 sa = SphereTerrainSurfaceVertices + (edge * Row) + k;
                const u32 sb = sa + 1;
                Emit(ea, eb, sa, outward);
                Emit(eb, sb, sa, outward);
            }
        }
        return indices;
    }

    vec2 MorphSphereTerrainGrid(const ivec2 grid, const f32 morph)
    {
        return vec2(grid) - (vec2(grid & ivec2(1)) * morph);
    }

    vec2 SphereTerrainParentGrid(const vec2 grid, const u32 quadrant)
    {
        const vec2 offset(static_cast<f32>(quadrant & 1u), static_cast<f32>((quadrant >> 1) & 1u));
        return (offset * static_cast<f32>(SphereTerrainGridQuads / 2)) + (grid * 0.5f);
    }

    f32 SphereTerrainMorphFactor(const f32 distance, const f32 start, const f32 end,
                                 const f32 floor)
    {
        const f32 span = std::max(end - start, 1e-6f);
        return std::max(std::clamp((distance - start) / span, 0.0f, 1.0f), floor);
    }

    vec4 SphereTerrainCornerWeights(const SphereTerrainTileConstants& tile, const vec2 st)
    {
        const f32 au = CubeSphere::TileFractionF32(st.x, tile.Warp.x, tile.Warp.y);
        const f32 av = CubeSphere::TileFractionF32(st.y, tile.Warp.z, tile.Warp.w);
        const vec4 beta =
            vec4((1.0f - au) * (1.0f - av), au * (1.0f - av), (1.0f - au) * av, au * av) *
            vec4(tile.Corners[0].w, tile.Corners[1].w, tile.Corners[2].w, tile.Corners[3].w);
        const f32 total = beta.x + beta.y + beta.z + beta.w;
        return beta / total;
    }

    SphereTerrainPlacement PlaceSphereTerrainVertex(const SphereTerrainDrawRecord& record,
                                                    const vec2 st, const f32 height)
    {
        const vec4 w = SphereTerrainCornerWeights(record.Tile, st);
        const f32 shortfall = (w.x * w.y * record.ChordsA.x) + (w.x * w.z * record.ChordsA.y) +
                              (w.x * w.w * record.ChordsA.z) + (w.y * w.z * record.ChordsA.w) +
                              (w.y * w.w * record.ChordsB.x) + (w.z * w.w * record.ChordsB.y);
        const f32 lift =
            (record.ChordsB.z * (shortfall / (1.0f + std::sqrt(1.0f - shortfall)))) + height;
        const vec3 vertical =
            glm::normalize((w.x * vec3(record.Verticals[0])) + (w.y * vec3(record.Verticals[1])) +
                           (w.z * vec3(record.Verticals[2])) + (w.w * vec3(record.Verticals[3])));
        const vec3 chord = (w.x * vec3(record.Corners[0])) + (w.y * vec3(record.Corners[1])) +
                           (w.z * vec3(record.Corners[2])) + (w.w * vec3(record.Corners[3]));
        return {.Position = chord + (vertical * lift), .Vertical = vertical};
    }
}
