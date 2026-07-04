#include <Veng/Renderer/PointField.h>

#include <Veng/Assert.h>
#include <Veng/Renderer/Buffer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace Veng::Renderer
{
    namespace
    {
        // A grid cell coordinate: the point's world position floored by the cell size on each axis.
        struct CellCoord
        {
            i32 X;
            i32 Y;
            i32 Z;

            bool operator==(const CellCoord& other) const
            {
                return X == other.X && Y == other.Y && Z == other.Z;
            }
        };

        struct CellCoordHash
        {
            usize operator()(const CellCoord& c) const
            {
                // A 64-bit integer mix over the three axes — plenty for a bucketing map.
                const u64 h = (static_cast<u64>(static_cast<u32>(c.X)) * 0x9E3779B97F4A7C15ULL) ^
                              (static_cast<u64>(static_cast<u32>(c.Y)) * 0xC2B2AE3D27D4EB4FULL) ^
                              (static_cast<u64>(static_cast<u32>(c.Z)) * 0x165667B19E3779F9ULL);
                return static_cast<usize>(h);
            }
        };

        CellCoord ToCell(const vec3 position, const f32 cellSize)
        {
            return CellCoord{
                .X = static_cast<i32>(std::floor(position.x / cellSize)),
                .Y = static_cast<i32>(std::floor(position.y / cellSize)),
                .Z = static_cast<i32>(std::floor(position.z / cellSize)),
            };
        }

        std::span<const u8> AsByteSpan(const std::span<const FieldPoint> points)
        {
            return std::span<const u8>(reinterpret_cast<const u8*>(points.data()),
                                       points.size_bytes());
        }
    }

    PointField::BuildData PointField::Bucket(const std::span<const FieldPoint> points,
                                             const f32 cellSize)
    {
        VE_ASSERT(cellSize > 0.0f, "PointField::Bucket: CellSize must be positive");

        // Group point indices by grid cell, then rewrite the set so each cell's points are a
        // contiguous run (a cell draws range [FirstPoint, FirstPoint+PointCount)).
        std::unordered_map<CellCoord, vector<u32>, CellCoordHash> buckets;
        buckets.reserve(points.size());
        for (u32 i = 0; i < points.size(); ++i)
        {
            buckets[ToCell(points[i].Position, cellSize)].push_back(i);
        }

        BuildData build;
        build.Points.reserve(points.size());
        build.Cells.reserve(buckets.size());
        for (const auto& [coord, indices] : buckets)
        {
            AABB cellBounds = AABB::Empty();
            vec3 centroidSum(0.0f);
            vec3 fluxSum(0.0f);
            const u32 first = static_cast<u32>(build.Points.size());
            for (const u32 index : indices)
            {
                const FieldPoint& point = points[index];
                build.Points.push_back(point);
                cellBounds.Expand(point.Position);
                centroidSum += point.Position;

                // Unpack RGBA8 (little-endian: R low byte) to linear [0,1] RGB, then weight by the
                // disc area (Size^2) — the constant-surface-brightness flux the aggregate spreads.
                const f32 inv = 1.0f / 255.0f;
                const vec3 color(static_cast<f32>(point.ColorRgba8 & 0xFFu) * inv,
                                 static_cast<f32>((point.ColorRgba8 >> 8) & 0xFFu) * inv,
                                 static_cast<f32>((point.ColorRgba8 >> 16) & 0xFFu) * inv);
                fluxSum += color * (point.Size * point.Size);
            }
            const f32 count = static_cast<f32>(indices.size());
            build.Cells.push_back(Cell{
                .Bounds = cellBounds,
                .Centroid = centroidSum / count,
                .SummedFlux = fluxSum,
                .FirstPoint = first,
                .PointCount = static_cast<u32>(indices.size()),
            });
            build.Bounds.Expand(cellBounds);
        }
        return build;
    }

    Unique<PointField> PointField::Create(Context& context, const PointFieldInfo& info)
    {
        return Create(context, PointFieldBuildInfo{
                                   .Name = info.Name,
                                   .CellSize = info.CellSize,
                                   .Data = Bucket(info.Points, info.CellSize),
                               });
    }

    Unique<PointField> PointField::Create(Context& context, PointFieldBuildInfo info)
    {
        VE_ASSERT(info.Data.Points.size() <= MaxPoints,
                  "PointField::Create: {} points exceeds the resident cap of {} — a consumer with "
                  "a variable set caps its realized count to MaxPoints",
                  info.Data.Points.size(), MaxPoints);
        VE_ASSERT(info.CellSize > 0.0f, "PointField::Create: CellSize must be positive");

        auto field = Unique<PointField>(new PointField());
        field->m_Context = &context;
        field->m_CellSize = info.CellSize;
        field->m_PointCount = static_cast<u32>(info.Data.Points.size());
        field->m_Cells = std::move(info.Data.Cells);
        field->m_Bounds = info.Data.Bounds;

        // The resident buffer is sized to the actual point count (at least one point so the
        // allocation is never zero-byte). Host-visible + storage: the draw reads it as an SSBO and
        // a sub-range Write memcpys straight in. The build's points are already cell-sorted, so
        // the one upload lands the order the cells index into.
        const u64 byteSize = std::max<u64>(1, field->m_PointCount) * sizeof(FieldPoint);
        field->m_PointBuffer = Buffer::Create(context, {
                                                           .Name = info.Name + " Points",
                                                           .Size = byteSize,
                                                           .Usage = BufferUsage::Storage,
                                                       });
        if (field->m_PointCount > 0)
        {
            field->m_PointBuffer->UploadSync(AsByteSpan(info.Data.Points));
        }
        return field;
    }

    PointField::~PointField() = default;

    void PointField::Rebucket(const std::span<const FieldPoint> points)
    {
        BuildData build = Bucket(points, m_CellSize);
        m_Cells = std::move(build.Cells);
        m_Bounds = build.Bounds;

        // The buffer must hold the cell-sorted order the cells index into.
        if (!build.Points.empty())
        {
            m_PointBuffer->UploadSync(AsByteSpan(build.Points));
        }
    }

    void PointField::Write(const u32 firstPoint, const std::span<const FieldPoint> points)
    {
        VE_ASSERT(firstPoint + points.size() <= m_PointCount,
                  "PointField::Write: range [{}, {}) exceeds the resident count {}", firstPoint,
                  firstPoint + points.size(), m_PointCount);
        if (points.empty())
        {
            return;
        }

        // Read the whole resident set back, splice in the new range, and rebucket — the sub-range
        // may move points across cells, so the cull grid and the buffer order both refresh.
        const vector<u8> raw = m_PointBuffer->Download();
        vector<FieldPoint> all(m_PointCount);
        std::memcpy(all.data(), raw.data(), static_cast<usize>(m_PointCount) * sizeof(FieldPoint));
        std::memcpy(all.data() + firstPoint, points.data(), points.size() * sizeof(FieldPoint));
        Rebucket(all);
    }
}
