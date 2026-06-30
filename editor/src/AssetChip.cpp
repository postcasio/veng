#include "AssetChip.h"

#include "AssetDragPayload.h"
#include "AssetSourceIndex.h"

#include <Veng/UI/UI.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace VengEditor
{
    using namespace Veng;

    const char* AssetTypeName(AssetType type)
    {
        switch (type)
        {
        case AssetType::Raw:
            return "Raw";
        case AssetType::Texture:
            return "Texture";
        case AssetType::Mesh:
            return "Mesh";
        case AssetType::Shader:
            return "Shader";
        case AssetType::Material:
            return "Material";
        case AssetType::MaterialInstance:
            return "MaterialInstance";
        case AssetType::VertexLayout:
            return "VertexLayout";
        case AssetType::Prefab:
            return "Prefab";
        case AssetType::Level:
            return "Level";
        case AssetType::Skeleton:
            return "Skeleton";
        case AssetType::Animation:
            return "Animation";
        case AssetType::Environment:
            return "EnvironmentMap";
        }
        return "Unknown";
    }

    const char* AssetTypeGlyph(AssetType type)
    {
        switch (type)
        {
        case AssetType::Raw:
            return "RAW";
        case AssetType::Texture:
            return "TEX";
        case AssetType::Mesh:
            return "MSH";
        case AssetType::Shader:
            return "SHD";
        case AssetType::Material:
            return "MAT";
        case AssetType::MaterialInstance:
            return "MTI";
        case AssetType::VertexLayout:
            return "VTX";
        case AssetType::Prefab:
            return "PFB";
        case AssetType::Level:
            return "LVL";
        case AssetType::Skeleton:
            return "SKL";
        case AssetType::Animation:
            return "ANM";
        case AssetType::Environment:
            return "ENV";
        }
        return "?";
    }

    vec4 AssetTypeColor(AssetType type)
    {
        switch (type)
        {
        case AssetType::Texture:
            return {0.85f, 0.55f, 0.25f, 1.0f};
        case AssetType::Mesh:
            return {0.30f, 0.55f, 0.85f, 1.0f};
        case AssetType::Material:
            return {0.40f, 0.70f, 0.40f, 1.0f};
        case AssetType::MaterialInstance:
            return {0.45f, 0.80f, 0.50f, 1.0f};
        case AssetType::Shader:
            return {0.60f, 0.45f, 0.80f, 1.0f};
        case AssetType::Prefab:
            return {0.30f, 0.70f, 0.70f, 1.0f};
        case AssetType::Level:
            return {0.75f, 0.45f, 0.55f, 1.0f};
        case AssetType::VertexLayout:
            return {0.55f, 0.55f, 0.60f, 1.0f};
        case AssetType::Skeleton:
            return {0.80f, 0.40f, 0.40f, 1.0f};
        case AssetType::Animation:
            return {0.50f, 0.65f, 0.30f, 1.0f};
        case AssetType::Environment:
            return {0.35f, 0.50f, 0.75f, 1.0f};
        case AssetType::Raw:
            return {0.50f, 0.50f, 0.50f, 1.0f};
        }
        return {0.50f, 0.50f, 0.50f, 1.0f};
    }

    namespace
    {
        // Source filename with the cooked extension(s) stripped (foo.tex.json -> foo).
        string StripCookedExtensions(const path& relativeSource)
        {
            path name = relativeSource.filename().stem();
            if (name.has_extension())
            {
                name = name.stem();
            }
            return name.string();
        }

        string ToLower(string_view text)
        {
            string lower(text);
            std::ranges::transform(lower, lower.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            return lower;
        }
    }

    string AssetDisplayName(AssetId id, const AssetSourceIndex& sources)
    {
        if (const AssetSourceIndex::Entry* source = sources.Find(id))
        {
            return StripCookedExtensions(source->RelativeSource);
        }
        return fmt::format("0x{:X}", id.Value);
    }

    namespace
    {
        // Per-chip search text and pending-focus set, keyed on the chip's id scope. The editor
        // draws on the single render thread, so neither map needs synchronization.
        std::unordered_map<string, string>& PickerFilters()
        {
            static std::unordered_map<string, string> s_Filters;
            return s_Filters;
        }

        std::unordered_set<string>& PickerFocusPending()
        {
            static std::unordered_set<string> s_Pending;
            return s_Pending;
        }

        // A subtle outline so the box reads as a contained unit against the panel background;
        // brightened while the box is hovered as a pickable affordance.
        constexpr vec4 ChipBorderColor{0.40f, 0.40f, 0.44f, 1.0f};
        constexpr vec4 ChipBorderHoverColor{0.55f, 0.60f, 0.70f, 1.0f};
        // Inset between the box border and the badge/text content.
        constexpr f32 ChipPad = 6.0f;
        // Outer breathing room above and below the box, so the chip is not flush against a tight
        // container (a property-table cell has only a couple of pixels of cell padding).
        constexpr f32 ChipMargin = 4.0f;

        struct ChipMetrics
        {
            f32 Tile = 0.0f;
            f32 Height = 0.0f;
            f32 Width = 0.0f;
        };

        // The chip is a square icon tile spanning three text lines, padded on every side; the
        // box never shrinks below the tile plus padding for a label.
        ChipMetrics MeasureChip(f32 requestedWidth)
        {
            const f32 lineHeight = UI::GetTextLineHeight();
            ChipMetrics metrics;
            metrics.Tile = lineHeight * 3.0f;
            metrics.Height = metrics.Tile + (ChipPad * 2.0f);
            const f32 floorWidth = metrics.Tile + (ChipPad * 3.0f);
            metrics.Width = requestedWidth < floorWidth ? floorWidth : requestedWidth;
            return metrics;
        }

        // Overlays the icon badge and the name/type/id text lines inside the box that begins at
        // @p origin (drawn over the already-reserved box item). The cursor is left on the last
        // text line — a final SetCursorPos must never close the region, so the box's reserving
        // item below sets its layout height.
        void OverlayChipContent(vec2 origin, AssetId id, AssetType type, string_view name,
                                const ChipMetrics& metrics)
        {
            const f32 lineHeight = UI::GetTextLineHeight();

            UI::SetCursorPos(vec2{origin.x + ChipPad, origin.y + ChipPad});
            UI::Badge(AssetTypeGlyph(type), AssetTypeColor(type), vec2{metrics.Tile, metrics.Tile});

            const f32 textX = origin.x + ChipPad + metrics.Tile + ChipPad;
            UI::SetCursorPos(vec2{textX, origin.y + ChipPad});
            if (id.IsValid())
            {
                UI::Text(name);
            }
            else
            {
                UI::TextDisabled("(none)");
            }
            UI::SetCursorPos(vec2{textX, origin.y + ChipPad + lineHeight});
            UI::TextDisabled(AssetTypeName(type));
            UI::SetCursorPos(vec2{textX, origin.y + ChipPad + (lineHeight * 2.0f)});
            UI::TextDisabled(id.IsValid() ? fmt::format("0x{:X}", id.Value) : string{"—"});
        }

        // Draws a self-contained chip preview (border + badge + text) for a drag tooltip; a
        // Dummy reserves the box rectangle that the content overlays.
        void DrawChipPreview(AssetId id, AssetType type, string_view name, f32 width)
        {
            const ChipMetrics metrics = MeasureChip(width);
            const vec2 origin = UI::CursorPos();
            UI::Dummy(vec2{metrics.Width, metrics.Height});
            UI::ItemBorder(ChipBorderColor, 1.0f);
            OverlayChipContent(origin, id, type, name, metrics);
        }
    }

    optional<AssetId> DrawAssetChip(const AssetChipInfo& info, const AssetSourceIndex& sources)
    {
        const string scope(info.IdScope);
        auto idScope = UI::PushId(scope);

        const string name =
            info.Name.empty() ? AssetDisplayName(info.Id, sources) : string(info.Name);
        const f32 requestedWidth = info.Width > 0.0f ? info.Width : UI::ContentRegionAvail().x;
        const ChipMetrics metrics = MeasureChip(requestedWidth);

        const vec2 origin = UI::CursorPos();
        const vec2 boxOrigin{origin.x, origin.y + ChipMargin};

        // The box is inset from the cell top by the outer margin; an invisible button of the box
        // size is the interactive base — it reports the click, anchors the drag/drop and border,
        // and the content overlays it. An invisible button (unlike a selectable) reserves its
        // rect exactly, with no half-item-spacing expansion, so ItemBorder frames the box tightly.
        UI::SetCursorPos(boxOrigin);
        const bool clicked = UI::InvisibleButton("##chipbox", vec2{metrics.Width, metrics.Height});
        const vec4 borderColor = UI::ItemHovered() ? ChipBorderHoverColor : ChipBorderColor;
        UI::ItemBorder(borderColor, 1.0f);

        optional<AssetId> result;

        // A drag source carries the asset's id + type and previews itself as a fixed-width chip.
        if (info.DragSource && info.Id.IsValid())
        {
            if (auto source = UI::DragDropSource())
            {
                const AssetDragPayload payload{.Id = info.Id, .Type = info.Type};
                UI::SetDragDropPayload(AssetPayload, &payload, sizeof(payload));
                DrawChipPreview(info.Id, info.Type, name, 220.0f);
            }
        }

        // A drop target accepts a same-type asset dropped onto the box.
        if (info.DropTarget)
        {
            if (auto target = UI::DragDropTarget())
            {
                if (const void* payload = UI::AcceptDragDropPayload(AssetPayload))
                {
                    AssetDragPayload dropped{};
                    std::memcpy(&dropped, payload, sizeof(dropped));
                    if (dropped.Type == info.Type)
                    {
                        result = dropped.Id;
                    }
                }
            }
        }

        OverlayChipContent(boxOrigin, info.Id, info.Type, name, metrics);

        // Reserve the full outer rectangle (margin + box + margin) as the final layout item, so
        // the row grows to include the bottom margin and the cursor lands below it. The Dummy is
        // inert, so input still resolves to the selectable beneath it.
        UI::SetCursorPos(origin);
        UI::Dummy(vec2{metrics.Width, metrics.Height + (ChipMargin * 2.0f)});

        // A drop-target chip doubles as a selector: clicking opens a search/pick popup.
        const char* popupId = "##assetpick";
        if (info.DropTarget && clicked)
        {
            UI::OpenPopup(popupId);
            PickerFocusPending().insert(scope);
        }

        if (info.DropTarget)
        {
            if (auto popup = UI::Popup(popupId))
            {
                string& filter = PickerFilters()[scope];
                UI::SetNextItemWidth(260.0f);
                if (PickerFocusPending().erase(scope) != 0)
                {
                    UI::SetKeyboardFocusHere();
                }
                (void)UI::InputTextWithHint("##search", "Search", filter);

                const string lowered = ToLower(filter);
                if (auto list = UI::Child("##candidates", vec2{280.0f, 320.0f}))
                {
                    if (UI::Selectable("(none)##clear"))
                    {
                        result = AssetId{};
                        UI::CloseCurrentPopup();
                    }
                    for (const AssetId candidate : sources.EntriesOfType(info.Type))
                    {
                        const string candidateName = AssetDisplayName(candidate, sources);
                        if (!lowered.empty() &&
                            ToLower(candidateName).find(lowered) == string::npos)
                        {
                            continue;
                        }
                        UI::Badge(AssetTypeGlyph(info.Type), AssetTypeColor(info.Type));
                        UI::SameLine();
                        const bool selected = candidate.Value == info.Id.Value;
                        if (UI::Selectable(
                                fmt::format("{}##cand{}", candidateName, candidate.Value),
                                selected))
                        {
                            result = candidate;
                            UI::CloseCurrentPopup();
                        }
                        UI::Tooltip(fmt::format("0x{:X}", candidate.Value));
                    }
                }
            }
        }

        return result;
    }
}
