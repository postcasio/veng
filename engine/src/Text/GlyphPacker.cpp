#include "GlyphPacker.h"

#include <Veng/Assert.h>

#include <algorithm>
#include <limits>

namespace Veng::Text
{
    GlyphPacker::GlyphPacker(const Config& config) : m_Config(config)
    {
        VE_ASSERT(m_Config.PageSize > 0, "GlyphPacker: page size must be positive");
        VE_ASSERT(m_Config.MaxPagesPerFieldType > 0, "GlyphPacker: page cap must be positive");
    }

    void GlyphPacker::BeginFrame(u64 frame)
    {
        VE_ASSERT(frame >= m_Frame, "GlyphPacker: frame epoch must not go backwards");
        m_Frame = frame;
    }

    std::optional<GlyphPacker::Slot> GlyphPacker::Touch(const GlyphKey& key)
    {
        const auto it = m_Entries.find(key);
        if (it == m_Entries.end())
        {
            return std::nullopt;
        }
        it->second.LastFrame = m_Frame;
        return MakeSlot(it->second);
    }

    std::optional<uvec2> GlyphPacker::PlaceOnPage(Page& page, const uvec2 alloc)
    {
        // Best-area fit: the tightest free rectangle that holds the allocation, so a large free
        // rectangle is kept whole for a glyph that actually needs it.
        usize best = page.Free.size();
        u32 bestArea = std::numeric_limits<u32>::max();
        for (usize i = 0; i < page.Free.size(); i++)
        {
            const FreeRect& rect = page.Free[i];
            if (rect.Size.x < alloc.x || rect.Size.y < alloc.y)
            {
                continue;
            }
            const u32 area = rect.Size.x * rect.Size.y;
            if (area < bestArea)
            {
                bestArea = area;
                best = i;
            }
        }
        if (best == page.Free.size())
        {
            return std::nullopt;
        }

        const FreeRect chosen = page.Free[best];
        page.Free[best] = page.Free.back();
        page.Free.pop_back();

        // Guillotine split, keeping the larger remainder in one piece so the free list stays as
        // unfragmented as this scheme allows: one cut runs full width below the allocation and the
        // other full height beside it, and the axis whose full-span piece is larger is the one taken
        // whole. The pieces are disjoint (the shorter one is clipped to the allocation's span), so
        // the free list never overlaps.
        const u32 leftoverW = chosen.Size.x - alloc.x;
        const u32 leftoverH = chosen.Size.y - alloc.y;
        const bool bottomSpansWidth = static_cast<u64>(chosen.Size.x) * leftoverH >=
                                      static_cast<u64>(leftoverW) * chosen.Size.y;
        if (leftoverW > 0)
        {
            page.Free.push_back(
                FreeRect{.Min = {chosen.Min.x + alloc.x, chosen.Min.y},
                         .Size = {leftoverW, bottomSpansWidth ? alloc.y : chosen.Size.y}});
        }
        if (leftoverH > 0)
        {
            page.Free.push_back(
                FreeRect{.Min = {chosen.Min.x, chosen.Min.y + alloc.y},
                         .Size = {bottomSpansWidth ? chosen.Size.x : alloc.x, leftoverH}});
        }
        return chosen.Min;
    }

    void GlyphPacker::FreeOnPage(Page& page, const uvec2 offset, const uvec2 alloc)
    {
        page.Free.push_back(FreeRect{.Min = offset, .Size = alloc});
    }

    u32 GlyphPacker::AllocatePage(const GlyphFieldType fieldType)
    {
        const auto index = static_cast<u32>(m_Pages.size());
        Page page;
        page.FieldType = fieldType;
        page.Free.push_back(
            FreeRect{.Min = {0, 0}, .Size = {m_Config.PageSize, m_Config.PageSize}});
        m_Pages.push_back(std::move(page));
        return index;
    }

    u32 GlyphPacker::PageCountFor(const GlyphFieldType fieldType) const
    {
        u32 count = 0;
        for (const Page& page : m_Pages)
        {
            if (page.FieldType == fieldType)
            {
                count++;
            }
        }
        return count;
    }

    GlyphPacker::Slot GlyphPacker::MakeSlot(const Entry& entry) const
    {
        return Slot{
            .Page = entry.Page,
            .Offset = entry.Offset,
            .Size = entry.GlyphSize,
            .FieldType = entry.FieldType,
            .Empty = entry.Empty,
            .PlaneMin = entry.M.PlaneMin,
            .PlaneMax = entry.M.PlaneMax,
            .Advance = entry.M.Advance,
            .LastFrame = entry.LastFrame,
            .ResidentFrame = entry.ResidentFrame,
        };
    }

    GlyphPacker::Result GlyphPacker::Insert(const GlyphKey& key, const Metrics& metrics)
    {
        VE_ASSERT(!m_Entries.contains(key), "GlyphPacker: Insert on an already-resident glyph");

        Result result;

        // A whitespace glyph is resident immediately and holds no page rectangle.
        if (metrics.Size.x == 0 || metrics.Size.y == 0)
        {
            Entry entry;
            entry.FieldType = key.FieldType;
            entry.Empty = true;
            entry.M = metrics;
            entry.LastFrame = m_Frame;
            entry.ResidentFrame = m_Frame;
            const Entry& stored = m_Entries.emplace(key, entry).first->second;
            result.Outcome = Outcome::Packed;
            result.Slot = MakeSlot(stored);
            return result;
        }

        const uvec2 alloc{metrics.Size.x + m_Config.Gutter, metrics.Size.y + m_Config.Gutter};

        // A glyph larger than a whole page (including its gutter) can never be packed.
        if (alloc.x > m_Config.PageSize || alloc.y > m_Config.PageSize)
        {
            result.Outcome = Outcome::NotResident;
            return result;
        }

        const GlyphFieldType field = key.FieldType;

        const auto place = [&](const u32 pageIndex, const uvec2 offset)
        {
            Entry entry;
            entry.Page = pageIndex;
            entry.Offset = offset;
            entry.GlyphSize = metrics.Size;
            entry.AllocSize = alloc;
            entry.FieldType = field;
            entry.M = metrics;
            entry.LastFrame = m_Frame;
            entry.ResidentFrame = m_Frame;
            const Entry& stored = m_Entries.emplace(key, entry).first->second;
            result.Outcome = Outcome::Packed;
            result.Slot = MakeSlot(stored);
        };

        // 1. An existing page of the field type with room.
        for (u32 i = 0; i < m_Pages.size(); i++)
        {
            if (m_Pages[i].FieldType != field)
            {
                continue;
            }
            if (const std::optional<uvec2> offset = PlaceOnPage(m_Pages[i], alloc))
            {
                place(i, *offset);
                return result;
            }
        }

        // 2. A fresh page, if the field type is under its cap.
        if (PageCountFor(field) < m_Config.MaxPagesPerFieldType)
        {
            const u32 pageIndex = AllocatePage(field);
            result.NewPage = true;
            result.NewPageIndex = pageIndex;
            result.NewPageFieldType = field;
            const std::optional<uvec2> offset = PlaceOnPage(m_Pages[pageIndex], alloc);
            VE_ASSERT(offset.has_value(),
                      "GlyphPacker: a glyph that fits a page did not fit a fresh one");
            place(pageIndex, *offset);
            return result;
        }

        // 3. Evict prior-frame glyphs, one page at a time, until the glyph fits. A page is exhausted
        // when it holds nothing but this-frame-pinned glyphs; the surplus then draws .notdef.
        for (u32 i = 0; i < m_Pages.size(); i++)
        {
            if (m_Pages[i].FieldType != field)
            {
                continue;
            }
            while (true)
            {
                if (const std::optional<uvec2> offset = PlaceOnPage(m_Pages[i], alloc))
                {
                    place(i, *offset);
                    return result;
                }

                // The least-recently-used evictable glyph on this page.
                auto victim = m_Entries.end();
                for (auto it = m_Entries.begin(); it != m_Entries.end(); ++it)
                {
                    const Entry& entry = it->second;
                    if (entry.Empty || entry.Page != i || entry.LastFrame == m_Frame)
                    {
                        continue;
                    }
                    if (victim == m_Entries.end() || entry.LastFrame < victim->second.LastFrame)
                    {
                        victim = it;
                    }
                }
                if (victim == m_Entries.end())
                {
                    break; // nothing evictable on this page — try the next.
                }

                FreeOnPage(m_Pages[i], victim->second.Offset, victim->second.AllocSize);
                result.Evicted.push_back(victim->first);
                result.Freed.push_back(FreedRect{
                    .Page = i, .Offset = victim->second.Offset, .Size = victim->second.AllocSize});
                m_Entries.erase(victim);
            }
        }

        result.Outcome = Outcome::NotResident;
        return result;
    }

    bool GlyphPacker::Contains(const GlyphKey& key) const
    {
        return m_Entries.contains(key);
    }

    u32 GlyphPacker::PageCount() const
    {
        return static_cast<u32>(m_Pages.size());
    }

    GlyphFieldType GlyphPacker::PageFieldType(const u32 page) const
    {
        VE_ASSERT(page < m_Pages.size(), "GlyphPacker: page index out of range");
        return m_Pages[page].FieldType;
    }

    usize GlyphPacker::ResidentCount() const
    {
        return m_Entries.size();
    }
}
