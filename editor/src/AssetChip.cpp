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

    string AssetTypeName(const AssetTypeRegistry& types, AssetTypeId type)
    {
        return types.GetDisplayName(type);
    }

    string AssetTypeGlyph(const AssetTypeRegistry& types, AssetTypeId type)
    {
        return types.GetGlyph(type);
    }

    vec4 AssetTypeColor(AssetTypeId type)
    {
        // Badge tints for the engine's own types. A game-registered type has no authored tint
        // and falls through to the neutral grey, so the badge still reads as a badge.
        static const std::unordered_map<AssetTypeId, vec4> s_Colors{
            {AssetTypes::Texture, {0.85f, 0.55f, 0.25f, 1.0f}},
            {AssetTypes::Mesh, {0.30f, 0.55f, 0.85f, 1.0f}},
            {AssetTypes::Material, {0.40f, 0.70f, 0.40f, 1.0f}},
            {AssetTypes::MaterialInstance, {0.45f, 0.80f, 0.50f, 1.0f}},
            {AssetTypes::Shader, {0.60f, 0.45f, 0.80f, 1.0f}},
            {AssetTypes::Prefab, {0.30f, 0.70f, 0.70f, 1.0f}},
            {AssetTypes::Level, {0.75f, 0.45f, 0.55f, 1.0f}},
            {AssetTypes::VertexLayout, {0.55f, 0.55f, 0.60f, 1.0f}},
            {AssetTypes::Skeleton, {0.80f, 0.40f, 0.40f, 1.0f}},
            {AssetTypes::Animation, {0.50f, 0.65f, 0.30f, 1.0f}},
            {AssetTypes::Environment, {0.35f, 0.50f, 0.75f, 1.0f}},
            {AssetTypes::InputMap, {0.70f, 0.60f, 0.35f, 1.0f}},
            {AssetTypes::Font, {0.65f, 0.35f, 0.65f, 1.0f}},
            {AssetTypes::StyleSheet, {0.40f, 0.60f, 0.75f, 1.0f}},
            {AssetTypes::UIDocument, {0.75f, 0.55f, 0.40f, 1.0f}},
            {AssetTypes::Raw, {0.50f, 0.50f, 0.50f, 1.0f}},
        };

        const auto it = s_Colors.find(type);
        return it == s_Colors.end() ? vec4{0.50f, 0.50f, 0.50f, 1.0f} : it->second;
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
        void OverlayChipContent(vec2 origin, AssetId id, AssetTypeId type, string_view name,
                                const ChipMetrics& metrics, const AssetTypeRegistry& types)
        {
            const f32 lineHeight = UI::GetTextLineHeight();

            UI::SetCursorPos(vec2{origin.x + ChipPad, origin.y + ChipPad});
            UI::Badge(AssetTypeGlyph(types, type), AssetTypeColor(type),
                      vec2{metrics.Tile, metrics.Tile});

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
            UI::TextDisabled(AssetTypeName(types, type));
            UI::SetCursorPos(vec2{textX, origin.y + ChipPad + (lineHeight * 2.0f)});
            UI::TextDisabled(id.IsValid() ? fmt::format("0x{:X}", id.Value) : string{"—"});
        }

        // Draws a self-contained chip preview (border + badge + text) for a drag tooltip; a
        // Dummy reserves the box rectangle that the content overlays.
        void DrawChipPreview(AssetId id, AssetTypeId type, string_view name, f32 width,
                             const AssetTypeRegistry& types)
        {
            const ChipMetrics metrics = MeasureChip(width);
            const vec2 origin = UI::CursorPos();
            UI::Dummy(vec2{metrics.Width, metrics.Height});
            UI::ItemBorder(ChipBorderColor, 1.0f);
            OverlayChipContent(origin, id, type, name, metrics, types);
        }
    }

    optional<AssetId> DrawAssetChip(const AssetChipInfo& info, const AssetSourceIndex& sources)
    {
        const AssetTypeRegistry& types = sources.GetAssetTypes();
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
                DrawChipPreview(info.Id, info.Type, name, 220.0f, types);
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

        OverlayChipContent(boxOrigin, info.Id, info.Type, name, metrics, types);

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
                        UI::Badge(AssetTypeGlyph(types, info.Type), AssetTypeColor(info.Type));
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
