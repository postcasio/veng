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

    Unique<PointField> PointField::Create(Context& context, const PointFieldInfo& info)
    {
        VE_ASSERT(info.Points.size() <= MaxPoints,
                  "PointField::Create: {} points exceeds the resident cap of {} — a consumer with "
                  "a variable set caps its realized count to MaxPoints",
                  info.Points.size(), MaxPoints);
        VE_ASSERT(info.CellSize > 0.0f, "PointField::Create: CellSize must be positive");

        auto field = Unique<PointField>(new PointField());
        field->m_Context = &context;
        field->m_CellSize = info.CellSize;
        field->m_PointCount = static_cast<u32>(info.Points.size());

        // The resident buffer is sized to the actual point count (at least one point so the
        // allocation is never zero-byte). Host-visible + storage: the draw reads it as an SSBO and
        // a sub-range Write memcpys straight in.
        const u64 byteSize = std::max<u64>(1, field->m_PointCount) * sizeof(FieldPoint);
        field->m_PointBuffer = Buffer::Create(context, {
                                                           .Name = info.Name + " Points",
                                                           .Size = byteSize,
                                                           .Usage = BufferUsage::Storage,
                                                       });

        if (field->m_PointCount > 0)
        {
            field->m_PointBuffer->UploadSync(AsByteSpan(info.Points));
        }
        field->Rebucket(info.Points);
        return field;
    }

    PointField::~PointField() = default;

    void PointField::Rebucket(const std::span<const FieldPoint> points)
    {
        m_Bounds = AABB::Empty();
        m_Cells.clear();

        // Group point indices by grid cell, then rewrite the buffer so each cell's points are a
        // contiguous run (a cell draws range [FirstPoint, FirstPoint+PointCount)).
        std::unordered_map<CellCoord, vector<u32>, CellCoordHash> buckets;
        buckets.reserve(points.size());
        for (u32 i = 0; i < points.size(); ++i)
        {
            buckets[ToCell(points[i].Position, m_CellSize)].push_back(i);
        }

        vector<FieldPoint> sorted;
        sorted.reserve(points.size());
        m_Cells.reserve(buckets.size());
        for (const auto& [coord, indices] : buckets)
        {
            AABB cellBounds = AABB::Empty();
            vec3 centroidSum(0.0f);
            vec3 fluxSum(0.0f);
            const u32 first = static_cast<u32>(sorted.size());
            for (const u32 index : indices)
            {
                const FieldPoint& point = points[index];
                sorted.push_back(point);
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
            m_Cells.push_back(Cell{
                .Bounds = cellBounds,
                .Centroid = centroidSum / count,
                .SummedFlux = fluxSum,
                .FirstPoint = first,
                .PointCount = static_cast<u32>(indices.size()),
            });
            m_Bounds.Expand(cellBounds);
        }

        // The buffer must hold the cell-sorted order the cells index into.
        if (!sorted.empty())
        {
            m_PointBuffer->UploadSync(AsByteSpan(sorted));
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
