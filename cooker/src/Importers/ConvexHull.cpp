#include "ConvexHull.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace Veng::Cook
{
    namespace
    {
        /// @brief Grid cell size the weld quantizes positions onto, in metres.
        ///
        /// A tenth of a millimetre: finer than any collision detail matters at, coarse enough that
        /// an exporter's last-bit differences between two copies of the same corner collapse.
        constexpr f64 WeldGrid = 1.0e-4;

        /// @brief A position quantized onto the weld grid, used as the weld map's key.
        struct GridKey
        {
            /// @brief Grid coordinate along x.
            i64 X = 0;
            /// @brief Grid coordinate along y.
            i64 Y = 0;
            /// @brief Grid coordinate along z.
            i64 Z = 0;

            /// @brief Member-wise equality, so the key works in an unordered map.
            bool operator==(const GridKey&) const = default;
        };

        /// @brief Hashes a GridKey by folding its three coordinates.
        struct GridKeyHash
        {
            /// @brief Returns the hash of a grid key.
            /// @param key  The key to hash.
            usize operator()(const GridKey& key) const noexcept
            {
                const usize a = std::hash<i64>{}(key.X);
                const usize b = std::hash<i64>{}(key.Y);
                const usize c = std::hash<i64>{}(key.Z);
                return a ^ (b << 1U) ^ (c << 2U);
            }
        };

        /// @brief Quantizes a position onto the weld grid.
        /// @param point  The position to quantize.
        [[nodiscard]] GridKey ToGridKey(const vec3 point)
        {
            return GridKey{
                .X = static_cast<i64>(std::llround(static_cast<f64>(point.x) / WeldGrid)),
                .Y = static_cast<i64>(std::llround(static_cast<f64>(point.y) / WeldGrid)),
                .Z = static_cast<i64>(std::llround(static_cast<f64>(point.z) / WeldGrid)),
            };
        }

        /// @brief Welds a point cloud, keeping the first position seen in each grid cell.
        /// @param points  The source point cloud.
        /// @param remap   Optional destination for source index → welded index; ignored when null.
        /// @return The welded positions, in first-seen order.
        [[nodiscard]] vector<vec3> WeldPoints(const std::span<const vec3> points,
                                              vector<u32>* remap)
        {
            std::unordered_map<GridKey, u32, GridKeyHash> seen;
            seen.reserve(points.size());
            vector<vec3> welded;
            welded.reserve(points.size());
            if (remap != nullptr)
            {
                remap->clear();
                remap->reserve(points.size());
            }

            for (const vec3 point : points)
            {
                const auto [entry, inserted] =
                    seen.try_emplace(ToGridKey(point), static_cast<u32>(welded.size()));
                if (inserted)
                {
                    welded.emplace_back(point);
                }
                if (remap != nullptr)
                {
                    remap->emplace_back(entry->second);
                }
            }
            return welded;
        }

        /// @brief One hull face: a triangle of vertex indices with its outward plane.
        struct HullFace
        {
            /// @brief The face's vertex indices, wound so Normal points out of the hull.
            u32 Index[3] = {};
            /// @brief The outward unit normal.
            dvec3 Normal{0.0};
            /// @brief The plane offset: a point p is outside when dot(Normal, p) > Offset.
            f64 Offset = 0.0;
        };

        /// @brief Builds a face over three vertices, oriented away from an interior point.
        /// @param points    The vertex set the indices address.
        /// @param a         First vertex index.
        /// @param b         Second vertex index.
        /// @param c         Third vertex index.
        /// @param interior  A point known to be inside the hull.
        /// @return The oriented face.
        [[nodiscard]] HullFace MakeFace(const vector<dvec3>& points, const u32 a, const u32 b,
                                        const u32 c, const dvec3 interior)
        {
            HullFace face{.Index = {a, b, c}};
            dvec3 normal = glm::cross(points[b] - points[a], points[c] - points[a]);
            const f64 length = glm::length(normal);
            normal = length > 0.0 ? normal / length : dvec3(0.0, 1.0, 0.0);
            f64 offset = glm::dot(normal, points[a]);
            if (glm::dot(normal, interior) > offset)
            {
                normal = -normal;
                offset = -offset;
                std::swap(face.Index[1], face.Index[2]);
            }
            face.Normal = normal;
            face.Offset = offset;
            return face;
        }

        /// @brief A directed edge of a face, used to find the horizon of a visible region.
        struct Edge
        {
            /// @brief The edge's first vertex index.
            u32 From = 0;
            /// @brief The edge's second vertex index.
            u32 To = 0;

            /// @brief Member-wise equality.
            bool operator==(const Edge&) const = default;
        };

        /// @brief Hashes a directed edge.
        struct EdgeHash
        {
            /// @brief Returns the hash of a directed edge.
            /// @param edge  The edge to hash.
            usize operator()(const Edge& edge) const noexcept
            {
                return (static_cast<usize>(edge.From) << 32U) ^ edge.To;
            }
        };
    }

    bool BuildConvexHull(const std::span<const vec3> points, vector<vec3>& out)
    {
        out.clear();
        const vector<vec3> welded = WeldPoints(points, nullptr);
        if (welded.size() < 4)
        {
            out = welded;
            return true;
        }

        vector<dvec3> exact;
        exact.reserve(welded.size());
        for (const vec3 point : welded)
        {
            exact.emplace_back(point);
        }

        // The plane tolerance scales with the cloud's extent, so a millimetre-scale prop and a
        // kilometre-scale structure both get a meaningful "in front of this face" test.
        dvec3 low = exact.front();
        dvec3 high = exact.front();
        for (const dvec3& point : exact)
        {
            low = glm::min(low, point);
            high = glm::max(high, point);
        }
        const dvec3 span = high - low;
        const f64 extent = std::max({span.x, span.y, span.z});
        const f64 epsilon = std::max(1.0e-9, extent * 1.0e-7);

        // Seed a tetrahedron: two extreme points, the point farthest off their line, then the
        // point farthest off their plane. Any step that collapses means the cloud has no volume.
        u32 a = 0;
        for (u32 i = 1; i < exact.size(); ++i)
        {
            if (exact[i].x < exact[a].x)
            {
                a = i;
            }
        }
        u32 b = a;
        f64 best = 0.0;
        for (u32 i = 0; i < exact.size(); ++i)
        {
            const f64 distance = glm::length(exact[i] - exact[a]);
            if (distance > best)
            {
                best = distance;
                b = i;
            }
        }
        if (best <= epsilon)
        {
            out = welded;
            return true;
        }

        u32 c = a;
        best = 0.0;
        const dvec3 axis = glm::normalize(exact[b] - exact[a]);
        for (u32 i = 0; i < exact.size(); ++i)
        {
            const dvec3 offset = exact[i] - exact[a];
            const f64 distance = glm::length(offset - axis * glm::dot(offset, axis));
            if (distance > best)
            {
                best = distance;
                c = i;
            }
        }
        if (best <= epsilon)
        {
            out = welded;
            return true;
        }

        u32 d = a;
        best = 0.0;
        const dvec3 planeNormal =
            glm::normalize(glm::cross(exact[b] - exact[a], exact[c] - exact[a]));
        for (u32 i = 0; i < exact.size(); ++i)
        {
            const f64 distance = std::abs(glm::dot(exact[i] - exact[a], planeNormal));
            if (distance > best)
            {
                best = distance;
                d = i;
            }
        }
        if (best <= epsilon)
        {
            out = welded;
            return true;
        }

        const dvec3 interior = (exact[a] + exact[b] + exact[c] + exact[d]) * 0.25;
        vector<HullFace> faces;
        faces.emplace_back(MakeFace(exact, a, b, c, interior));
        faces.emplace_back(MakeFace(exact, a, b, d, interior));
        faces.emplace_back(MakeFace(exact, a, c, d, interior));
        faces.emplace_back(MakeFace(exact, b, c, d, interior));

        vector<HullFace> kept;
        std::unordered_map<Edge, u32, EdgeHash> visibleEdges;
        for (u32 i = 0; i < exact.size(); ++i)
        {
            if (i == a || i == b || i == c || i == d)
            {
                continue;
            }
            const dvec3& point = exact[i];

            visibleEdges.clear();
            kept.clear();
            for (const HullFace& face : faces)
            {
                if (glm::dot(face.Normal, point) > face.Offset + epsilon)
                {
                    visibleEdges.try_emplace(Edge{.From = face.Index[0], .To = face.Index[1]}, 0);
                    visibleEdges.try_emplace(Edge{.From = face.Index[1], .To = face.Index[2]}, 0);
                    visibleEdges.try_emplace(Edge{.From = face.Index[2], .To = face.Index[0]}, 0);
                }
                else
                {
                    kept.emplace_back(face);
                }
            }
            if (visibleEdges.empty())
            {
                continue;
            }

            // The horizon is exactly the directed edges of the visible region whose reverse is not
            // itself visible: an interior edge is shared by two visible faces in both directions.
            faces = kept;
            for (const auto& [edge, unused] : visibleEdges)
            {
                if (visibleEdges.contains(Edge{.From = edge.To, .To = edge.From}))
                {
                    continue;
                }
                faces.emplace_back(MakeFace(exact, edge.From, edge.To, i, interior));
            }

            if (faces.size() > 2 * MaxConvexHullPoints)
            {
                return false;
            }
        }

        vector<bool> used(exact.size(), false);
        for (const HullFace& face : faces)
        {
            used[face.Index[0]] = true;
            used[face.Index[1]] = true;
            used[face.Index[2]] = true;
        }
        for (u32 i = 0; i < welded.size(); ++i)
        {
            if (used[i])
            {
                out.emplace_back(welded[i]);
            }
        }
        return out.size() <= MaxConvexHullPoints;
    }

    void WeldTriangleMesh(const std::span<const vec3> points, const std::span<const u32> indices,
                          vector<vec3>& outPoints, vector<u32>& outIndices)
    {
        vector<u32> remap;
        outPoints = WeldPoints(points, &remap);

        outIndices.clear();
        outIndices.reserve(indices.size());
        for (usize i = 0; i + 2 < indices.size(); i += 3)
        {
            const u32 a = remap[indices[i]];
            const u32 b = remap[indices[i + 1]];
            const u32 c = remap[indices[i + 2]];
            if (a == b || b == c || c == a)
            {
                continue;
            }
            outIndices.emplace_back(a);
            outIndices.emplace_back(b);
            outIndices.emplace_back(c);
        }
    }
}
