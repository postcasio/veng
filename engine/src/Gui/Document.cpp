#include <Veng/Gui/Document.h>

#include "FillGeometry.h"
#include "YogaTree.h"

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Font.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Gui/Placement.h>
#include <Veng/Gui/StyleSheet.h>
#include <Veng/Gui/UIDocument.h>
#include <Veng/Reflection/EnumName.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Viewport.h>

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <unordered_map>

namespace Veng::Gui
{
    namespace
    {
        YGFlexDirection ToYogaDirection(FlexDirection direction)
        {
            switch (direction)
            {
            case FlexDirection::Row:
                return YGFlexDirectionRow;
            case FlexDirection::RowReverse:
                return YGFlexDirectionRowReverse;
            case FlexDirection::Column:
                return YGFlexDirectionColumn;
            case FlexDirection::ColumnReverse:
                return YGFlexDirectionColumnReverse;
            }
            return YGFlexDirectionColumn;
        }

        YGJustify ToYogaJustify(Justify justify)
        {
            switch (justify)
            {
            case Justify::FlexStart:
                return YGJustifyFlexStart;
            case Justify::Center:
                return YGJustifyCenter;
            case Justify::FlexEnd:
                return YGJustifyFlexEnd;
            case Justify::SpaceBetween:
                return YGJustifySpaceBetween;
            case Justify::SpaceAround:
                return YGJustifySpaceAround;
            case Justify::SpaceEvenly:
                return YGJustifySpaceEvenly;
            }
            return YGJustifyFlexStart;
        }

        YGAlign ToYogaAlign(Align align)
        {
            switch (align)
            {
            case Align::Auto:
                return YGAlignAuto;
            case Align::FlexStart:
                return YGAlignFlexStart;
            case Align::Center:
                return YGAlignCenter;
            case Align::FlexEnd:
                return YGAlignFlexEnd;
            case Align::Stretch:
                return YGAlignStretch;
            }
            return YGAlignAuto;
        }

        YGWrap ToYogaWrap(FlexWrap wrap)
        {
            switch (wrap)
            {
            case FlexWrap::NoWrap:
                return YGWrapNoWrap;
            case FlexWrap::Wrap:
                return YGWrapWrap;
            case FlexWrap::WrapReverse:
                return YGWrapWrapReverse;
            }
            return YGWrapNoWrap;
        }

        // Applies a sizing length to a node through the three per-length setters (auto/points/percent).
        template <typename SetPoints, typename SetPercent, typename SetAuto>
        void ApplyLength(Length length, SetPoints setPoints, SetPercent setPercent, SetAuto setAuto)
        {
            switch (length.Kind)
            {
            case LengthKind::Auto:
                setAuto();
                return;
            case LengthKind::Points:
                setPoints(length.Value);
                return;
            case LengthKind::Percent:
                setPercent(length.Value);
                return;
            }
        }

        // Decodes UTF-8 into Unicode codepoints, emitting U+FFFD for a malformed byte so an
        // ill-formed string degrades to replacement glyphs rather than reading out of bounds.
        vector<u32> DecodeUtf8(string_view text)
        {
            vector<u32> codepoints;
            codepoints.reserve(text.size());

            usize i = 0;
            while (i < text.size())
            {
                const u8 lead = static_cast<u8>(text[i]);
                u32 codepoint = 0;
                usize extra = 0;

                if (lead < 0x80)
                {
                    codepoint = lead;
                }
                else if ((lead & 0xE0) == 0xC0)
                {
                    codepoint = lead & 0x1F;
                    extra = 1;
                }
                else if ((lead & 0xF0) == 0xE0)
                {
                    codepoint = lead & 0x0F;
                    extra = 2;
                }
                else if ((lead & 0xF8) == 0xF0)
                {
                    codepoint = lead & 0x07;
                    extra = 3;
                }
                else
                {
                    codepoints.push_back(0xFFFD);
                    ++i;
                    continue;
                }

                if (i + extra >= text.size())
                {
                    codepoints.push_back(0xFFFD);
                    break;
                }

                bool valid = true;
                for (usize k = 1; k <= extra; ++k)
                {
                    const u8 cont = static_cast<u8>(text[i + k]);
                    if ((cont & 0xC0) != 0x80)
                    {
                        valid = false;
                        break;
                    }
                    codepoint = (codepoint << 6) | (cont & 0x3F);
                }

                if (valid)
                {
                    codepoints.push_back(codepoint);
                    i += extra + 1;
                }
                else
                {
                    codepoints.push_back(0xFFFD);
                    ++i;
                }
            }

            return codepoints;
        }

        // Appends one Unicode codepoint to a UTF-8 string, the inverse of DecodeUtf8; an
        // out-of-range codepoint is emitted as U+FFFD so a round-trip stays well-formed.
        void AppendUtf8(string& out, u32 codepoint)
        {
            if (codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
            {
                codepoint = 0xFFFD;
            }
            if (codepoint < 0x80)
            {
                out.push_back(static_cast<char>(codepoint));
            }
            else if (codepoint < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            else if (codepoint < 0x10000)
            {
                out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
        }

        // Adds or removes a single interaction-state bit from a mask.
        ElementState WithBit(ElementState mask, ElementState bit, bool set)
        {
            const u32 m = static_cast<u32>(mask);
            const u32 b = static_cast<u32>(bit);
            return static_cast<ElementState>(set ? (m | b) : (m & ~b));
        }

        void ApplyEdgeInsets(YGNodeRef node, const Insets& insets,
                             void (*setter)(YGNodeRef, YGEdge, float))
        {
            setter(node, YGEdgeLeft, insets.Left);
            setter(node, YGEdgeTop, insets.Top);
            setter(node, YGEdgeRight, insets.Right);
            setter(node, YGEdgeBottom, insets.Bottom);
        }

        // Measures a text leaf: the Document's override when set, else its style's resident font.
        YGSize MeasureText(YGNodeConstRef node, float width, YGMeasureMode widthMode,
                           float /*height*/, YGMeasureMode /*heightMode*/)
        {
            const auto* element = static_cast<const Element*>(YGNodeGetContext(node));
            const auto* document = static_cast<const Document*>(
                YGConfigGetContext(YGNodeGetConfig(const_cast<YGNodeRef>(node))));
            if (element == nullptr || document == nullptr)
            {
                return {.width = 0.0f, .height = 0.0f};
            }

            const optional<f32> maxWidth =
                widthMode == YGMeasureModeUndefined ? optional<f32>{} : optional<f32>{width};

            const vec2 size = document->MeasureElementText(*element, maxWidth);
            return {.width = size.x, .height = size.y};
        }

        // Measures an Image leaf: its texture's own pixels, so an Image with no authored size lays
        // out at natural scale and flexes like any other measured content. A sliced Image measures
        // the sum of its corner insets instead — the smallest box at which the frame still reads,
        // its center collapsed to nothing. An unresolved texture measures zero rather than
        // asserting, the tolerance an un-textured Image already gets at paint.
        YGSize MeasureImage(YGNodeConstRef node, float /*width*/, YGMeasureMode /*widthMode*/,
                            float /*height*/, YGMeasureMode /*heightMode*/)
        {
            const auto* element = static_cast<const Element*>(YGNodeGetContext(node));
            if (element == nullptr)
            {
                return {.width = 0.0f, .height = 0.0f};
            }

            const Insets& slice = element->ComputedStyle.ImageSlice;
            if (slice.Left > 0.0f || slice.Top > 0.0f || slice.Right > 0.0f || slice.Bottom > 0.0f)
            {
                return {.width = slice.Left + slice.Right, .height = slice.Top + slice.Bottom};
            }
            return {.width = element->ImageSize.x, .height = element->ImageSize.y};
        }
    }

    Document::Document()
    {
        m_Yoga = CreateUnique<YogaTree>();
        YGConfigSetContext(m_Yoga->Config, this);
        m_Root = &CreateElement(ElementKind::Panel);
    }

    Document::~Document()
    {
        // Self-detach from the hosting viewport's layer stack through the stored back-reference, so
        // dropping the owning Unique is the whole of cleanup with no dangling pointer left behind.
        if (m_HostViewport != nullptr)
        {
            m_HostViewport->DetachDocument(*this);
        }
    }

    namespace
    {
        // The selector type-name of an element kind — the same tag the markup authors and the
        // cooked StyleRule::Type stores, so a runtime match against a resolved rule is by name.
        const char* ElementKindName(ElementKind kind)
        {
            switch (kind)
            {
            case ElementKind::Panel:
                return "Panel";
            case ElementKind::Text:
                return "Text";
            case ElementKind::Image:
                return "Image";
            case ElementKind::Button:
                return "Button";
            case ElementKind::Checkbox:
                return "Checkbox";
            case ElementKind::Slider:
                return "Slider";
            case ElementKind::ProgressBar:
                return "ProgressBar";
            case ElementKind::TextInput:
                return "TextInput";
            case ElementKind::ScrollView:
                return "ScrollView";
            case ElementKind::List:
                return "List";
            case ElementKind::Table:
                return "Table";
            case ElementKind::ScrollBar:
                return "ScrollBar";
            case ElementKind::ScrollBarThumb:
                return "ScrollBarThumb";
            case ElementKind::SliderFill:
                return "SliderFill";
            case ElementKind::SliderThumb:
                return "SliderThumb";
            }
            return "Panel";
        }

        // Whether an axis scrolls: clipped content the user can move. Hidden clips without moving.
        bool ScrollsAxis(Overflow overflow)
        {
            return overflow == Overflow::Scroll;
        }

        // Whether an element scrolls on either axis — the style-driven successor to the
        // `Kind == ScrollView` check. A ScrollView is simply an element whose overflow defaults to
        // Scroll, so a List, Table, or Panel styled `overflow-y: scroll` scrolls identically.
        bool IsScrollable(const Style& style)
        {
            return ScrollsAxis(style.OverflowX) || ScrollsAxis(style.OverflowY);
        }

        // Whether an element clips its content. A scissor is a whole-box rect, so either axis
        // being non-Visible clips the box.
        bool ClipsContent(const Style& style)
        {
            return style.OverflowX != Overflow::Visible || style.OverflowY != Overflow::Visible;
        }

        // A box a texture fill occupies, with the corner radii that box carries.
        struct FillBox
        {
            Rect Box;
            CornerRadii Radii;
        };

        // Deflates a rect by per-edge insets, clamping each axis's pair so the box never inverts and
        // reducing each corner radius by the larger of its two adjacent insets — the CSS
        // inner-radius rule, so the deflated corner stays inside the outer one. All-zero insets are
        // the identity.
        FillBox DeflateBox(const Rect& rect, const CornerRadii& radii, const Insets& by)
        {
            const auto fit = [](f32 near, f32 far, f32 extent)
            {
                const f32 total = std::max(near, 0.0f) + std::max(far, 0.0f);
                const f32 scale = total > extent && total > 0.0f ? extent / total : 1.0f;
                return vec2(std::max(near, 0.0f) * scale, std::max(far, 0.0f) * scale);
            };
            const vec2 x = fit(by.Left, by.Right, rect.Size.x);
            const vec2 y = fit(by.Top, by.Bottom, rect.Size.y);
            return FillBox{
                .Box = Rect{.Min = rect.Min + vec2(x.x, y.x),
                            .Size = rect.Size - vec2(x.x + x.y, y.x + y.y)},
                .Radii =
                    CornerRadii{
                        .TopLeft = std::max(radii.TopLeft - std::max(x.x, y.x), 0.0f),
                        .TopRight = std::max(radii.TopRight - std::max(x.y, y.x), 0.0f),
                        .BottomRight = std::max(radii.BottomRight - std::max(x.y, y.y), 0.0f),
                        .BottomLeft = std::max(radii.BottomLeft - std::max(x.x, y.y), 0.0f),
                    },
            };
        }

        // The reserved frame thickness. A negative authored border-width is not a frame, so every
        // consumer — the layout reservation, the two box deflations, and the paint-side text and
        // caret insets — reads the width through here and agrees on a malformed value.
        f32 BorderWidth(const Style& style)
        {
            return std::max(style.BorderStyle.Width, 0.0f);
        }

        // The padding box: the border box less the border ring, so a background image sits behind
        // the border and the content exactly as CSS paints one.
        FillBox ToPaddingBox(const Rect& rect, const Style& style)
        {
            const f32 width = BorderWidth(style);
            return DeflateBox(rect, style.Radii,
                              Insets{.Left = width, .Top = width, .Right = width, .Bottom = width});
        }

        // The content box: the padding box less the padding, the box a measured leaf's own content
        // is laid out and painted in — the same border+padding origin the text runs draw from.
        FillBox ToContentBox(const Rect& rect, const Style& style)
        {
            const f32 width = BorderWidth(style);
            const Insets& padding = style.Padding;
            return DeflateBox(rect, style.Radii,
                              Insets{.Left = width + padding.Left,
                                     .Top = width + padding.Top,
                                     .Right = width + padding.Right,
                                     .Bottom = width + padding.Bottom});
        }

        // Whether an element is a widget-owned part rather than authored content — a scrollbar
        // track or thumb, a slider's fill or handle. A part is a real element so it styles through
        // the ordinary cascade, but it is not content and never appears in a content walk.
        // Whether a part carries a drag of its own. A scrollbar thumb does — it maps a pointer
        // delta through the track's slack — so it must claim a press before the walk reaches the
        // element it scrolls. A slider's parts do not: the Slider already maps pointer position to
        // value, so a press on its fill or thumb belongs to the Slider itself.
        bool IsScrollBarPart(ElementKind kind)
        {
            return kind == ElementKind::ScrollBar || kind == ElementKind::ScrollBarThumb;
        }

        bool IsWidgetPart(ElementKind kind)
        {
            switch (kind)
            {
            case ElementKind::ScrollBar:
            case ElementKind::ScrollBarThumb:
            case ElementKind::SliderFill:
            case ElementKind::SliderThumb:
                return true;
            default:
                return false;
            }
        }

        // An element's authored content children, excluding the widget-owned parts.
        //
        // The parts live in Children so they inherit the layout mirror, the cascade, the paint
        // order, and hit-testing rather than needing parallel paths — but they are not content, so
        // every content-shaped walk (item slots, focus order, template capture, table columns, the
        // list shrink, the scroll extent) goes through this one accessor rather than each testing
        // the kind itself. The parts are always appended after the content, so trimming the tail
        // is the whole exclusion.
        std::span<Element* const> ContentChildren(const Element& element)
        {
            usize count = element.Children.size();
            while (count > 0 && IsWidgetPart(element.Children[count - 1]->Kind))
            {
                --count;
            }
            return std::span<Element* const>(element.Children).first(count);
        }

        /// @brief The scrollbar thickness used when a bar's style declares no explicit width.
        constexpr f32 DefaultScrollBarThickness = 10.0f;

        /// @brief The shortest a thumb is allowed to become on a very long scroll range, in pixels.
        constexpr f32 MinScrollThumbLength = 24.0f;

        /// @brief The fill color a SliderFill paints with when no rule styles it.
        constexpr vec4 DefaultSliderFill{0.35f, 0.42f, 0.52f, 1.0f};

        /// @brief The fill color a SliderThumb paints with when no rule styles it.
        constexpr vec4 DefaultSliderThumb{0.85f, 0.88f, 0.92f, 1.0f};

        // A rule's selector matches an element when each constrained axis (type/class/id) agrees
        // and an unconstrained (empty) axis is a wildcard. A class constraint matches if the tag
        // is among the element's classes.
        bool RuleMatches(const StyleRule& rule, const Element& element)
        {
            if (!rule.Type.empty() && rule.Type != ElementKindName(element.Kind))
            {
                return false;
            }
            if (!rule.Id.empty() && rule.Id != element.Id)
            {
                return false;
            }
            if (!rule.Class.empty() &&
                std::ranges::find(element.Classes, rule.Class) == element.Classes.end())
            {
                return false;
            }
            return true;
        }

        // Cascades the None-state rule survivors of every referenced sheet onto the element's base
        // style (sheet reference order, then rule source order), then the inline overrides last, and
        // keeps each state-scoped survivor as a variant in the same source order. Inline always wins.
        // An `animation` declaration is element state, not a Style field: it copies the referenced
        // sheet clip's keyframes onto the element (a later declaration replaces an earlier one, the
        // cascade's last-wins), so the live document never borrows the sheet. A `transition`
        // declaration resolves the same way, copying its slice of the sheet's transition table onto
        // the element — the same list Document::SetTransitions writes, so both paths reach one
        // runtime mechanism.
        // Cascades the sheets onto an element, then its inline style over the top. `recipe` is
        // null for a widget-owned element, which has neither an authored identity nor inline style
        // — only the sheet rules its kind and classes match.
        void ResolveElementStyle(
            Element& element, const UIElementRecipe* recipe,
            const vector<const StyleSheet*>& sheets, AssetManager* assets,
            const function<optional<ResolvedGradient>(const StyleSheet&, u32)>& resolveGradient)
        {
            element.Variants.clear();
            element.Animations.clear();
            element.Transitions.clear();

            for (const StyleSheet* sheet : sheets)
            {
                for (const StyleRule& rule : sheet->GetRules())
                {
                    if (!RuleMatches(rule, element))
                    {
                        continue;
                    }
                    if (rule.State == ElementState::None)
                    {
                        for (const StyleDeclaration& declaration : rule.Declarations)
                        {
                            if (declaration.Property == StyleProperty::Animation)
                            {
                                const vector<StyleAnimationClip>& clips = sheet->GetAnimations();
                                if (declaration.Unit < clips.size())
                                {
                                    element.Animations.assign({StyleAnimation{
                                        .Keyframes = clips[declaration.Unit].Keyframes,
                                        .Duration = declaration.Values.x,
                                        .Mode = static_cast<AnimationLoopMode>(
                                            static_cast<u32>(declaration.Values.y))}});
                                }
                                continue;
                            }
                            if (declaration.Property == StyleProperty::Transition)
                            {
                                const vector<StyleTransition>& table = sheet->GetTransitions();
                                const auto first = static_cast<usize>(declaration.Unit);
                                const auto count = static_cast<usize>(declaration.Values.x);
                                if (first + count <= table.size())
                                {
                                    element.Transitions.assign(
                                        table.begin() + static_cast<isize>(first),
                                        table.begin() + static_cast<isize>(first + count));
                                }
                                continue;
                            }
                            if (declaration.Property == StyleProperty::BackgroundGradient)
                            {
                                // A gradient references the sheet's gradient table and uploads its
                                // ramp; resolve it here where the sheet is in hand (variants and
                                // inline styles, which reapply without a sheet, do not carry it).
                                if (optional<ResolvedGradient> resolved =
                                        resolveGradient(*sheet, declaration.Unit))
                                {
                                    element.BaseStyle.BackgroundGradient = std::move(resolved);
                                }
                                continue;
                            }
                            ApplyDeclaration(element.BaseStyle, declaration, assets);
                        }
                    }
                    else
                    {
                        element.Variants.push_back(
                            StyleVariant{.State = rule.State, .Declarations = rule.Declarations});
                    }
                }
            }

            if (recipe != nullptr)
            {
                for (const StyleDeclaration& declaration : recipe->InlineStyle)
                {
                    ApplyDeclaration(element.BaseStyle, declaration, assets);
                }
            }

            element.ComputedStyle = element.BaseStyle;
        }

        // Copies a recipe element's authored identity, text, bindings, and handlers onto a live
        // element. Bindings and handlers are stored on the element's Bindings map keyed by the bound
        // attribute name and the event name respectively (an event name never collides with a bound
        // attribute), unresolved for the binding/handler resolution step. Style resolution is a
        // separate step (ResolveElementStyle), run once the element's classes/id are in place.
        void PopulateElement(Element& element, const UIElementRecipe& recipe)
        {
            element.Id = recipe.Id;
            element.Classes = recipe.Classes;
            element.Text = recipe.Text;

            for (const UIBindingRecipe& binding : recipe.Bindings)
            {
                element.Bindings[binding.Property] = binding.Expression;
            }
            for (const UIHandlerRecipe& handler : recipe.Handlers)
            {
                element.Bindings[handler.Event] = handler.Handler;
            }
        }

        // Hands an Image's resolved source texture to the material shading it, once, through the
        // two conventional handle fields. A material that declares neither shades without the
        // texture (a purely procedural fill), so the write is conditional on the reflected schema
        // rather than required by it — and it runs at resolve, not per frame, since a UI material
        // has no per-frame parameter channel.
        void BindImageMaterialTexture(Element& element)
        {
            MaterialInstance* const material = element.ComputedStyle.ImageMaterial.Get();
            if (material == nullptr || !element.ImageTexture.IsValid())
            {
                return;
            }
            const auto declares = [material](const char* name, MaterialField::FieldKind kind)
            {
                return std::ranges::any_of(material->GetFields(), [&](const MaterialField& field)
                                           { return field.Name == name && field.Kind == kind; });
            };
            if (declares(ImageMaterialTextureField, MaterialField::FieldKind::TextureHandle))
            {
                material->SetTextureHandle(ImageMaterialTextureField, element.ImageTexture);
            }
            if (declares(ImageMaterialSamplerField, MaterialField::FieldKind::SamplerHandle))
            {
                material->SetSamplerHandle(ImageMaterialSamplerField, element.ImageSampler);
            }
        }

        // Resolves an Image element's source texture: its recipe tint and UV fold onto the element,
        // and its `src` AssetId resolves to a resident AssetHandle<Texture> through the borrowed
        // manager (a cache lookup — the texture is already resident as a load-time dependency), whose
        // bindless slots the paint samples. A missing or unresolved texture leaves the element
        // un-textured (its box paints, no crash), the same tolerance a missing font gets.
        void ResolveElementImage(Element& element, const UIElementRecipe& recipe,
                                 AssetManager& assets)
        {
            if (element.Kind != ElementKind::Image)
            {
                return;
            }
            element.ImageTint = recipe.Tint;
            element.ImageUv = recipe.Uv;
            if (!recipe.Src.IsValid())
            {
                return;
            }
            AssetHandle<Texture> texture =
                assets.LoadSync<Texture>(recipe.Src).value_or(AssetHandle<Texture>{});
            if (texture.IsLoaded())
            {
                element.ImageTexture = texture.Get()->GetHandle();
                element.ImageSampler = texture.Get()->GetSamplerHandle();
                element.ImageSize = vec2(texture.Get()->GetExtent());
                element.Image = std::move(texture);
            }

            BindImageMaterialTexture(element);
        }

        // Formats a leaf field value at `fieldPtr` (of registered type `info`) as display text. A
        // scalar prints its number, a string prints itself, an enum prints its enumerator name, a
        // vector prints its components space-separated. Scalars are keyed by their leaf TypeId (not
        // a name string), so the read matches the exact backing type. A field class with no scalar
        // text form (Struct/Variant/Array/Matrix/Reference/AssetHandle) yields nullopt, so a binding
        // onto one is a no-op rather than a garbage write.
        optional<string> FormatLeaf(const void* fieldPtr, const TypeInfo& info)
        {
            switch (info.Class)
            {
            case FieldClass::Scalar:
            {
                if (info.Id == TypeIdOf<bool>())
                {
                    return *static_cast<const bool*>(fieldPtr) ? string{"true"} : string{"false"};
                }
                if (info.Id == TypeIdOf<f32>())
                {
                    return fmt::format("{}", *static_cast<const f32*>(fieldPtr));
                }
                if (info.Id == TypeIdOf<i32>())
                {
                    return fmt::format("{}", *static_cast<const i32*>(fieldPtr));
                }
                if (info.Id == TypeIdOf<u32>())
                {
                    return fmt::format("{}", *static_cast<const u32*>(fieldPtr));
                }
                if (info.Id == TypeIdOf<u64>())
                {
                    return fmt::format("{}", *static_cast<const u64*>(fieldPtr));
                }
                return std::nullopt;
            }
            case FieldClass::String:
                return *static_cast<const string*>(fieldPtr);
            case FieldClass::Enum:
                return EnumeratorName(info, LoadEnumBits(fieldPtr, info));
            case FieldClass::Vector:
            {
                // A glm vector is a run of f32s; the byte width gives the component count.
                const usize count = info.Size / sizeof(f32);
                const auto* comps = static_cast<const f32*>(fieldPtr);
                string out;
                for (usize i = 0; i < count; ++i)
                {
                    if (i != 0)
                    {
                        out.push_back(' ');
                    }
                    out += fmt::format("{}", comps[i]);
                }
                return out;
            }
            default:
                return std::nullopt;
            }
        }

        // Walks a dotted field path ("a.b.c") from a struct base pointer through reflection, and
        // returns the leaf value formatted as text — nullopt when a segment is missing, an
        // intermediate segment is not a struct, or the leaf has no text form. Every segment but the
        // last must be a Struct-class field; the final segment is a formattable leaf.
        optional<string> ResolvePath(const TypeRegistry& registry, void* base, TypeId type,
                                     string_view path)
        {
            void* cursor = base;
            TypeId cursorType = type;

            usize start = 0;
            while (start <= path.size())
            {
                const usize dot = path.find('.', start);
                const string_view segment =
                    path.substr(start, dot == string_view::npos ? string_view::npos : dot - start);
                if (segment.empty() || !registry.IsRegistered(cursorType))
                {
                    return std::nullopt;
                }

                const TypeInfo& owner = registry.Info(cursorType);
                const FieldDescriptor* field = nullptr;
                for (const FieldDescriptor& candidate : owner.Fields)
                {
                    if (candidate.Name == segment)
                    {
                        field = &candidate;
                        break;
                    }
                }
                if (field == nullptr)
                {
                    return std::nullopt;
                }

                void* fieldPtr = static_cast<u8*>(cursor) + field->Offset;
                const bool last = dot == string_view::npos;
                if (last)
                {
                    if (!registry.IsRegistered(field->Type))
                    {
                        return std::nullopt;
                    }
                    return FormatLeaf(fieldPtr, registry.Info(field->Type));
                }

                // A non-terminal segment must be a struct to descend into.
                if (field->Class != FieldClass::Struct)
                {
                    return std::nullopt;
                }
                cursor = fieldPtr;
                cursorType = field->Type;
                start = dot + 1;
            }
            return std::nullopt;
        }

        // One resolved field: the storage pointer at the leaf and the descriptor describing it.
        struct ResolvedField
        {
            void* Ptr = nullptr;
            const FieldDescriptor* Field = nullptr;
        };

        // Walks a dotted field path and returns the leaf field's storage pointer and descriptor,
        // rather than its formatted text — the array-field variant of ResolvePath a List binds
        // through. nullopt when a segment is missing or an intermediate is not a struct.
        optional<ResolvedField> ResolveFieldPtr(const TypeRegistry& registry, void* base,
                                                TypeId type, string_view path)
        {
            void* cursor = base;
            TypeId cursorType = type;

            usize start = 0;
            while (start <= path.size())
            {
                const usize dot = path.find('.', start);
                const string_view segment =
                    path.substr(start, dot == string_view::npos ? string_view::npos : dot - start);
                if (segment.empty() || !registry.IsRegistered(cursorType))
                {
                    return std::nullopt;
                }

                const TypeInfo& owner = registry.Info(cursorType);
                const FieldDescriptor* field = nullptr;
                for (const FieldDescriptor& candidate : owner.Fields)
                {
                    if (candidate.Name == segment)
                    {
                        field = &candidate;
                        break;
                    }
                }
                if (field == nullptr)
                {
                    return std::nullopt;
                }

                void* fieldPtr = static_cast<u8*>(cursor) + field->Offset;
                const bool last = dot == string_view::npos;
                if (last)
                {
                    return ResolvedField{.Ptr = fieldPtr, .Field = field};
                }
                if (field->Class != FieldClass::Struct)
                {
                    return std::nullopt;
                }
                cursor = fieldPtr;
                cursorType = field->Type;
                start = dot + 1;
            }
            return std::nullopt;
        }

        // Whether a style property may be the target of a `{path}` binding: the paint properties
        // a model plausibly drives per frame. All are paint inputs, so a bound write never
        // re-dirties layout.
        bool IsBindableStyleProperty(StyleProperty property)
        {
            switch (property)
            {
            case StyleProperty::Background:
            case StyleProperty::BorderColor:
            case StyleProperty::TextColor:
            case StyleProperty::Opacity:
                return true;
            default:
                return false;
            }
        }

        // Reads a bound leaf field as a numeric style payload: an f32 in x, a glm vector in its
        // components (a vec3 color promotes alpha 1). Nullopt for any other leaf shape. Colors are
        // taken as authored — linear straight-alpha, the draw-list contract.
        optional<vec4> ResolveNumericLeaf(const TypeRegistry& registry, void* base, TypeId type,
                                          string_view path)
        {
            const optional<ResolvedField> field = ResolveFieldPtr(registry, base, type, path);
            if (!field || !registry.IsRegistered(field->Field->Type))
            {
                return std::nullopt;
            }
            const TypeInfo& info = registry.Info(field->Field->Type);
            if (info.Class == FieldClass::Scalar && info.Id == TypeIdOf<f32>())
            {
                return vec4(*static_cast<const f32*>(field->Ptr), 0.0f, 0.0f, 0.0f);
            }
            if (info.Class == FieldClass::Vector)
            {
                const usize count = std::min<usize>(info.Size / sizeof(f32), 4);
                const auto* comps = static_cast<const f32*>(field->Ptr);
                vec4 value(0.0f, 0.0f, 0.0f, 1.0f);
                for (usize i = 0; i < count; ++i)
                {
                    value[static_cast<i32>(i)] = comps[i];
                }
                return value;
            }
            return std::nullopt;
        }
    }

    Unique<Document> Document::Instantiate(const UIDocument& recipe, AssetManager& assets)
    {
        auto document = CreateUnique<Document>();
        document->m_Assets = &assets;

        const vector<UIElementRecipe>& elements = recipe.GetElements();
        if (elements.empty())
        {
            return document;
        }

        document->m_StyleSheets = recipe.GetStyleSheets();
        vector<const StyleSheet*> sheets;
        sheets.reserve(document->m_StyleSheets.size());
        for (const AssetHandle<StyleSheet>& handle : document->m_StyleSheets)
        {
            if (handle.IsLoaded())
            {
                sheets.push_back(handle.Get());
            }
        }

        // A gradient declaration's ramp uploads through the borrowed manager into a small ramp
        // texture, cached per (sheet, gradient) so elements sharing one gradient share one texture.
        // The AssetHandle kept on each element's Style keeps the texture resident for its lifetime.
        auto& gradientCache = document->m_GradientCache;
        const function<optional<ResolvedGradient>(const StyleSheet&, u32)> resolveGradient =
            [&assets, &gradientCache](const StyleSheet& sheet,
                                      const u32 index) -> optional<ResolvedGradient>
        {
            const vector<StyleGradient>& gradients = sheet.GetGradients();
            if (index >= gradients.size())
            {
                return std::nullopt;
            }
            const StyleGradient& source = gradients[index];
            if (const auto it = gradientCache.find(&source); it != gradientCache.end())
            {
                return it->second;
            }
            const TextureData data{
                .Name = "gui-gradient-ramp",
                .Extent = uvec2(source.Width, 1),
                .Format = Renderer::Format::RGBA16Sfloat,
                .MipLevels = 1,
                .Pixels = source.Ramp,
                .Sampler =
                    Renderer::SamplerInfo{.AddressModeU = Renderer::AddressMode::ClampToEdge,
                                          .AddressModeV = Renderer::AddressMode::ClampToEdge,
                                          .AddressModeW = Renderer::AddressMode::ClampToEdge},
            };
            ResolvedGradient resolved{.Kind = source.Kind,
                                      .P0 = source.P0,
                                      .P1 = source.P1,
                                      .AngleOffset = source.AngleOffset,
                                      .Ramp = assets.BuildSync<Texture>(data)};
            gradientCache.emplace(&source, resolved);
            return resolved;
        };

        // Element 0 is the authored root; it maps onto the document's pre-made root. The pre-order
        // recipe carries an explicit child count per element, so a recursive walk over a shared
        // cursor rebuilds the hierarchy in one linear pass. A recipe root of a non-Panel kind still
        // populates the Panel root's fields — a document authors a Panel root in practice.
        usize cursor = 0;
        const auto build = [&](Element& live, auto&& self) -> void
        {
            const UIElementRecipe& node = elements[cursor];
            ++cursor;
            PopulateElement(live, node);
            ResolveElementStyle(live, &node, sheets, &assets, resolveGradient);
            ResolveElementImage(live, node, assets);
            document->InitWidget(live);
            for (u32 i = 0; i < node.ChildCount; ++i)
            {
                Element& child = document->Add(live, elements[cursor].Kind);
                self(child, self);
            }
        };
        build(document->Root(), build);

        // The cascaded base includes layout inputs, so the tree must re-solve.
        document->m_Dirty = true;
        return document;
    }

    namespace
    {
        Element* FindByIdRecursive(Element& element, const string_view id)
        {
            if (element.Id == id)
            {
                return &element;
            }
            for (Element* child : element.Children)
            {
                if (Element* const found = FindByIdRecursive(*child, id); found != nullptr)
                {
                    return found;
                }
            }
            return nullptr;
        }
    }

    Element* Document::FindById(const string_view id)
    {
        if (id.empty())
        {
            return nullptr;
        }
        return FindByIdRecursive(*m_Root, id);
    }

    const Element* Document::FindById(const string_view id) const
    {
        if (id.empty())
        {
            return nullptr;
        }
        return FindByIdRecursive(*m_Root, id);
    }

    namespace
    {
        template <class ElementT, class OutT>
        void FindAllByClassRecursive(ElementT& element, const string_view name, vector<OutT>& out)
        {
            if (std::ranges::find(element.Classes, name) != element.Classes.end())
            {
                out.push_back(&element);
            }
            for (auto* child : element.Children)
            {
                FindAllByClassRecursive(*child, name, out);
            }
        }
    }

    vector<Element*> Document::FindAllByClass(const string_view name)
    {
        vector<Element*> found;
        if (name.empty())
        {
            return found;
        }
        FindAllByClassRecursive(*m_Root, name, found);
        return found;
    }

    vector<const Element*> Document::FindAllByClass(const string_view name) const
    {
        vector<const Element*> found;
        if (name.empty())
        {
            return found;
        }
        FindAllByClassRecursive(*m_Root, name, found);
        return found;
    }

    Element& Document::Root()
    {
        return *m_Root;
    }

    const Element& Document::Root() const
    {
        return *m_Root;
    }

    Element& Document::CreateElement(ElementKind kind)
    {
        auto owned = CreateUnique<Element>();
        owned->Kind = kind;
        owned->Serial = m_NextSerial++;

        // ScrollView is the named preset over the overflow property: it is a Panel whose overflow
        // defaults to Scroll on both axes. Seeding the default here rather than forcing it at paint
        // means an authored `overflow-x: hidden` still wins, since the cascade runs over this base.
        if (kind == ElementKind::ScrollView)
        {
            owned->BaseStyle.OverflowX = Overflow::Scroll;
            owned->BaseStyle.OverflowY = Overflow::Scroll;
            owned->ComputedStyle.OverflowX = Overflow::Scroll;
            owned->ComputedStyle.OverflowY = Overflow::Scroll;
        }

        Element& element = *owned;
        m_Elements.push_back(std::move(owned));

        const YGNodeRef node = m_Yoga->Create(element);
        // Text, Button, and TextInput are text-sized leaves: each paints a run inside its own box,
        // so its intrinsic size is that run's shaped extent and it carries the text measure. (A
        // measured node cannot take children — Yoga asserts loudly.)
        if (kind == ElementKind::Text || kind == ElementKind::Button ||
            kind == ElementKind::TextInput)
        {
            YGNodeSetMeasureFunc(node, &MeasureText);
        }
        // An Image is the other measured leaf: its content is its texture, so its intrinsic size is
        // that texture's pixels and an Image with no authored width/height lays out at natural
        // scale instead of collapsing.
        else if (kind == ElementKind::Image)
        {
            YGNodeSetMeasureFunc(node, &MeasureImage);
        }
        return element;
    }

    Element& Document::Add(Element& parent, ElementKind kind)
    {
        Element& child = CreateElement(kind);
        child.Parent = &parent;

        // Scrollbar parts are a trailing tail of the child list, so they paint over the content and
        // ContentChildren can exclude them by trimming the end. A bar can be created before the
        // content it scrolls (InitWidget runs ahead of a List's first item sync), so content
        // inserts ahead of the tail rather than appending after it.
        const usize insertAt =
            IsWidgetPart(kind) ? parent.Children.size() : ContentChildren(parent).size();
        parent.Children.insert(parent.Children.begin() + static_cast<std::ptrdiff_t>(insertAt),
                               &child);

        const YGNodeRef parentNode = m_Yoga->Get(parent);
        const YGNodeRef childNode = m_Yoga->Get(child);
        VE_ASSERT(parentNode != nullptr && childNode != nullptr,
                  "Gui::Document::Add: parent element does not belong to this document");

        // A Button, TextInput, or Image is a measured leaf until it takes a child, at which point it
        // becomes a container sized by its children like a Panel — a measured Yoga node cannot
        // hold children. Text stays a hard leaf.
        if ((parent.Kind == ElementKind::Button || parent.Kind == ElementKind::TextInput ||
             parent.Kind == ElementKind::Image) &&
            YGNodeHasMeasureFunc(parentNode))
        {
            YGNodeSetMeasureFunc(parentNode, nullptr);
        }
        YGNodeInsertChild(parentNode, childNode,
                          std::min(insertAt, YGNodeGetChildCount(parentNode)));

        m_Dirty = true;
        return child;
    }

    void Document::DestroySubtree(Element& element)
    {
        ForgetElement(element);

        // Depth-first so a child's node is freed before its parent's.
        for (Element* child : element.Children)
        {
            DestroySubtree(*child);
        }

        m_Yoga->Destroy(element);

        // Drop any item template the element held: the map is keyed by element address, so a
        // stale entry would be re-adopted by whichever element the allocator next puts there.
        m_ListTemplates.erase(&element);

        const auto it = std::ranges::find_if(m_Elements, [&](const Unique<Element>& owned)
                                             { return owned.get() == &element; });
        if (it != m_Elements.end())
        {
            m_Elements.erase(it);
        }
    }

    void Document::Remove(Element& element)
    {
        VE_ASSERT(&element != m_Root, "Gui::Document::Remove: the root cannot be removed");

        Element* const parent = element.Parent;
        if (parent != nullptr)
        {
            const YGNodeRef parentNode = m_Yoga->Get(*parent);
            const YGNodeRef childNode = m_Yoga->Get(element);
            if (parentNode != nullptr && childNode != nullptr)
            {
                YGNodeRemoveChild(parentNode, childNode);
            }

            const auto it = std::ranges::find(parent->Children, &element);
            if (it != parent->Children.end())
            {
                parent->Children.erase(it);
            }
        }

        DestroySubtree(element);
        m_Dirty = true;
    }

    void Document::SetText(Element& element, string_view text)
    {
        if (element.Text == text)
        {
            return;
        }
        element.Text.assign(text);
        if (const YGNodeRef node = m_Yoga->Get(element);
            node != nullptr && YGNodeHasMeasureFunc(node))
        {
            YGNodeMarkDirty(node);
        }
        m_Dirty = true;
    }

    void Document::SetVisible(Element& element, bool visible)
    {
        element.Visible = visible;
        m_Dirty = true;
    }

    void Document::SetStyle(Element& element, const Style& style)
    {
        element.BaseStyle = style;
        element.ComputedStyle = style;
        element.Tweens.clear();
        MarkSubtreeTextDirty(element);
        m_Dirty = true;
    }

    void Document::SetOpacity(Element& element, const f32 opacity)
    {
        element.BaseStyle.Opacity = opacity;
        element.ComputedStyle.Opacity = opacity;
        m_PaintDirty = true;
    }

    void Document::SetRotation(Element& element, const f32 degrees)
    {
        element.BaseStyle.Rotation = degrees;
        element.ComputedStyle.Rotation = degrees;
        m_PaintDirty = true;
    }

    void Document::SetBackground(Element& element, const vec4 color)
    {
        element.BaseStyle.Background = color;
        element.ComputedStyle.Background = color;
        m_PaintDirty = true;
    }

    void Document::SetBackgroundGradient(Element& element, optional<ResolvedGradient> gradient)
    {
        element.BaseStyle.BackgroundGradient = gradient;
        element.ComputedStyle.BackgroundGradient = std::move(gradient);
        m_PaintDirty = true;
    }

    void Document::SetTextColor(Element& element, const vec4 color)
    {
        element.BaseStyle.TextColor = color;
        element.ComputedStyle.TextColor = color;
        m_PaintDirty = true;
    }

    void Document::SetImageUv(Element& element, const Rect& uv)
    {
        element.ImageUv = uv;
        m_PaintDirty = true;
    }

    void Document::SetPlacement(Element& element, const vec2 topLeft, const vec2 size)
    {
        const Style& base = element.BaseStyle;
        const bool unchanged =
            base.Position == PositionType::Absolute && base.Inset.Left == topLeft.x &&
            base.Inset.Top == topLeft.y && !PositionInsets::IsSet(base.Inset.Right) &&
            !PositionInsets::IsSet(base.Inset.Bottom) && base.Width.Kind == LengthKind::Points &&
            base.Width.Value == size.x && base.Height.Kind == LengthKind::Points &&
            base.Height.Value == size.y;
        if (unchanged)
        {
            return;
        }

        const auto place = [&](Style& style)
        {
            style.Position = PositionType::Absolute;
            style.Inset = {.Left = topLeft.x, .Top = topLeft.y};
            style.Width = Length::Points(size.x);
            style.Height = Length::Points(size.y);
        };
        place(element.BaseStyle);
        place(element.ComputedStyle);
        m_Dirty = true;
    }

    void Document::SetPinnedPosition(Element& element, const vec2 topLeft)
    {
        // Position and position type only: an auto-sized element writes no size, so folding one
        // into the unchanged-test would fail on every call and re-solve the whole tree per frame.
        const Style& base = element.BaseStyle;
        const bool unchanged = base.Position == PositionType::Absolute &&
                               base.Inset.Left == topLeft.x && base.Inset.Top == topLeft.y &&
                               !PositionInsets::IsSet(base.Inset.Right) &&
                               !PositionInsets::IsSet(base.Inset.Bottom);
        if (unchanged)
        {
            return;
        }

        const auto place = [&](Style& style)
        {
            style.Position = PositionType::Absolute;
            style.Inset = {.Left = topLeft.x, .Top = topLeft.y};
        };
        place(element.BaseStyle);
        place(element.ComputedStyle);
        m_Dirty = true;
    }

    void Document::SetAnimations(Element& element, vector<StyleAnimation> animations)
    {
        element.Animations = std::move(animations);
    }

    namespace
    {
        // Whether a property feeds the flexbox solve (or the text measure that feeds it) — a change
        // to one re-dirties the layout, where a pure paint change (color/opacity/radius) does not.
        bool IsLayoutProperty(StyleProperty property)
        {
            switch (property)
            {
            case StyleProperty::FlexDirection:
            case StyleProperty::JustifyContent:
            case StyleProperty::AlignItems:
            case StyleProperty::AlignSelf:
            case StyleProperty::FlexWrap:
            case StyleProperty::FlexGrow:
            case StyleProperty::FlexShrink:
            case StyleProperty::FlexBasis:
            case StyleProperty::Width:
            case StyleProperty::Height:
            case StyleProperty::MinWidth:
            case StyleProperty::MinHeight:
            case StyleProperty::MaxWidth:
            case StyleProperty::MaxHeight:
            case StyleProperty::Margin:
            case StyleProperty::Padding:
            case StyleProperty::Position:
            case StyleProperty::Inset:
            case StyleProperty::InsetLeft:
            case StyleProperty::InsetTop:
            case StyleProperty::InsetRight:
            case StyleProperty::InsetBottom:
            case StyleProperty::Origin:
            case StyleProperty::TextSize:
            case StyleProperty::TextFont:
                return true;
            case StyleProperty::Background:
            case StyleProperty::BackgroundGradient:
            case StyleProperty::CornerRadius:
            case StyleProperty::BorderWidth:
            case StyleProperty::BorderColor:
            case StyleProperty::TextColor:
            case StyleProperty::Opacity:
            case StyleProperty::Rotation:
            case StyleProperty::PointerEvents:
            case StyleProperty::Animation:
            case StyleProperty::TextAlign:
            case StyleProperty::TextWrap:
            case StyleProperty::BackgroundImage:
            case StyleProperty::BackgroundSlice:
            case StyleProperty::BackgroundFit:
            case StyleProperty::BackgroundRepeat:
            case StyleProperty::ObjectFit:
            case StyleProperty::ImageRepeat:
            case StyleProperty::BoxShadow:
            case StyleProperty::BoxShadowColor:
            case StyleProperty::BackgroundMaterial:
            case StyleProperty::ImageMaterial:
            case StyleProperty::Transition:
                return false;
            // A slice makes an Image's intrinsic size the sum of its corner insets, so authoring or
            // dropping one re-measures the leaf.
            case StyleProperty::ImageSlice:
                return true;
            // An overflow axis and the scrollbar layout move layout inputs: a gutter takes its
            // width out of the content box, and turning an axis scrollable re-clamps the offset.
            case StyleProperty::Overflow:
            case StyleProperty::OverflowX:
            case StyleProperty::OverflowY:
            case StyleProperty::ScrollbarLayout:
                return true;
            }
            return false;
        }

        // Whether a property's payload is a Length (value + kind ordinal), whose ease is valid only
        // within one kind — a Points→Percent change snaps rather than interpolating a mixed unit.
        bool IsLengthProperty(StyleProperty property)
        {
            switch (property)
            {
            case StyleProperty::FlexBasis:
            case StyleProperty::Width:
            case StyleProperty::Height:
            case StyleProperty::MinWidth:
            case StyleProperty::MinHeight:
            case StyleProperty::MaxWidth:
            case StyleProperty::MaxHeight:
                return true;
            default:
                return false;
            }
        }

        // Reads a property's numeric payload as a vec4. A Length rides {value, kind ordinal, 0, 0}
        // so a kind mismatch between source and target is detectable (kinds must agree to ease).
        vec4 ReadProperty(const Style& style, StyleProperty property)
        {
            const auto lengthVec = [](Length length)
            { return vec4(length.Value, static_cast<f32>(length.Kind), 0.0f, 0.0f); };

            switch (property)
            {
            case StyleProperty::FlexGrow:
                return vec4(style.FlexGrow, 0.0f, 0.0f, 0.0f);
            case StyleProperty::FlexShrink:
                return vec4(style.FlexShrink, 0.0f, 0.0f, 0.0f);
            case StyleProperty::FlexBasis:
                return lengthVec(style.FlexBasis);
            case StyleProperty::Width:
                return lengthVec(style.Width);
            case StyleProperty::Height:
                return lengthVec(style.Height);
            case StyleProperty::MinWidth:
                return lengthVec(style.MinWidth);
            case StyleProperty::MinHeight:
                return lengthVec(style.MinHeight);
            case StyleProperty::MaxWidth:
                return lengthVec(style.MaxWidth);
            case StyleProperty::MaxHeight:
                return lengthVec(style.MaxHeight);
            case StyleProperty::Margin:
                return vec4(style.Margin.Left, style.Margin.Top, style.Margin.Right,
                            style.Margin.Bottom);
            case StyleProperty::Padding:
                return vec4(style.Padding.Left, style.Padding.Top, style.Padding.Right,
                            style.Padding.Bottom);
            case StyleProperty::Inset:
                return vec4(style.Inset.Left, style.Inset.Top, style.Inset.Right,
                            style.Inset.Bottom);
            case StyleProperty::Background:
                return style.Background;
            case StyleProperty::CornerRadius:
                return vec4(style.Radii.TopLeft, style.Radii.TopRight, style.Radii.BottomRight,
                            style.Radii.BottomLeft);
            case StyleProperty::BorderWidth:
                return vec4(style.BorderStyle.Width, 0.0f, 0.0f, 0.0f);
            case StyleProperty::BorderColor:
                return style.BorderStyle.Color;
            case StyleProperty::TextColor:
                return style.TextColor;
            case StyleProperty::TextSize:
                return vec4(style.TextSize, 0.0f, 0.0f, 0.0f);
            case StyleProperty::Opacity:
                return vec4(style.Opacity, 0.0f, 0.0f, 0.0f);
            case StyleProperty::Rotation:
                return vec4(style.Rotation, 0.0f, 0.0f, 0.0f);
            case StyleProperty::InsetLeft:
                return vec4(style.Inset.Left, 0.0f, 0.0f, 0.0f);
            case StyleProperty::InsetTop:
                return vec4(style.Inset.Top, 0.0f, 0.0f, 0.0f);
            case StyleProperty::InsetRight:
                return vec4(style.Inset.Right, 0.0f, 0.0f, 0.0f);
            case StyleProperty::InsetBottom:
                return vec4(style.Inset.Bottom, 0.0f, 0.0f, 0.0f);
            case StyleProperty::Origin:
                return vec4(style.Origin.x, style.Origin.y, 0.0f, 0.0f);
            // Read but never written: the slice is not animatable, but it *is* a layout input, and
            // the layout-move scan compares styles through this reader.
            case StyleProperty::ImageSlice:
                return vec4(style.ImageSlice.Left, style.ImageSlice.Top, style.ImageSlice.Right,
                            style.ImageSlice.Bottom);
            default:
                return vec4(0.0f);
            }
        }

        // Writes an interpolated vec4 back onto the style's field, inverting ReadProperty. A Length's
        // kind rides component y.
        void WriteProperty(Style& style, StyleProperty property, vec4 value)
        {
            const auto toLength = [](vec4 v)
            {
                return Length{.Kind = static_cast<LengthKind>(static_cast<i32>(v.y)), .Value = v.x};
            };

            switch (property)
            {
            case StyleProperty::FlexGrow:
                style.FlexGrow = value.x;
                return;
            case StyleProperty::FlexShrink:
                style.FlexShrink = value.x;
                return;
            case StyleProperty::FlexBasis:
                style.FlexBasis = toLength(value);
                return;
            case StyleProperty::Width:
                style.Width = toLength(value);
                return;
            case StyleProperty::Height:
                style.Height = toLength(value);
                return;
            case StyleProperty::MinWidth:
                style.MinWidth = toLength(value);
                return;
            case StyleProperty::MinHeight:
                style.MinHeight = toLength(value);
                return;
            case StyleProperty::MaxWidth:
                style.MaxWidth = toLength(value);
                return;
            case StyleProperty::MaxHeight:
                style.MaxHeight = toLength(value);
                return;
            case StyleProperty::Margin:
                style.Margin = {
                    .Left = value.x, .Top = value.y, .Right = value.z, .Bottom = value.w};
                return;
            case StyleProperty::Padding:
                style.Padding = {
                    .Left = value.x, .Top = value.y, .Right = value.z, .Bottom = value.w};
                return;
            case StyleProperty::Inset:
                style.Inset = {
                    .Left = value.x, .Top = value.y, .Right = value.z, .Bottom = value.w};
                return;
            case StyleProperty::Background:
                style.Background = value;
                return;
            case StyleProperty::CornerRadius:
                style.Radii = {.TopLeft = value.x,
                               .TopRight = value.y,
                               .BottomRight = value.z,
                               .BottomLeft = value.w};
                return;
            case StyleProperty::BorderWidth:
                style.BorderStyle.Width = value.x;
                return;
            case StyleProperty::BorderColor:
                style.BorderStyle.Color = value;
                return;
            case StyleProperty::TextColor:
                style.TextColor = value;
                return;
            case StyleProperty::TextSize:
                style.TextSize = value.x;
                return;
            case StyleProperty::Opacity:
                style.Opacity = value.x;
                return;
            case StyleProperty::Rotation:
                style.Rotation = value.x;
                return;
            case StyleProperty::InsetLeft:
                style.Inset.Left = value.x;
                return;
            case StyleProperty::InsetTop:
                style.Inset.Top = value.x;
                return;
            case StyleProperty::InsetRight:
                style.Inset.Right = value.x;
                return;
            case StyleProperty::InsetBottom:
                style.Inset.Bottom = value.x;
                return;
            case StyleProperty::Origin:
                style.Origin = vec2(value.x, value.y);
                return;
            default:
                return;
            }
        }

        // Folds the variants whose state bit is set in `state` over the base style, in stored source
        // order (later-listed states win — the USS order), producing the resolved target style.
        Style ResolveTarget(const Element& element, AssetManager* assets)
        {
            Style target = element.BaseStyle;
            for (const StyleVariant& variant : element.Variants)
            {
                if ((element.State & variant.State) == ElementState::None)
                {
                    continue;
                }
                for (const StyleDeclaration& declaration : variant.Declarations)
                {
                    ApplyDeclaration(target, declaration, assets);
                }
            }
            return target;
        }

        // Maps an animation's clock onto its clip as a normalized [0, 1] phase per its loop mode.
        f32 AnimationPhase(const StyleAnimation& animation)
        {
            if (animation.Duration <= 0.0f)
            {
                return 1.0f;
            }
            const f32 cycles = animation.Time / animation.Duration;
            switch (animation.Mode)
            {
            case AnimationLoopMode::Loop:
                return cycles - std::floor(cycles);
            case AnimationLoopMode::PingPong:
            {
                const f32 wrapped = cycles - 2.0f * std::floor(cycles * 0.5f);
                return wrapped < 1.0f ? wrapped : 2.0f - wrapped;
            }
            case AnimationLoopMode::Once:
                return std::min(cycles, 1.0f);
            }
            return 1.0f;
        }

        // Writes one animated property at `phase` onto the live style: the value interpolates
        // between the bracketing keyframes that declare the property; a property declared on one
        // side only — or a non-animatable / unit-mismatched pair — snaps to the nearer declared
        // keyframe (the earlier one between brackets, so a discrete value flips at its next key).
        void ApplyAnimatedProperty(Style& live, const StyleAnimation& animation,
                                   const StyleProperty property, const f32 phase,
                                   AssetManager* assets)
        {
            const StyleDeclaration* before = nullptr;
            const StyleDeclaration* after = nullptr;
            f32 beforeOffset = 0.0f;
            f32 afterOffset = 1.0f;
            for (const StyleKeyframe& key : animation.Keyframes)
            {
                for (const StyleDeclaration& declaration : key.Declarations)
                {
                    if (declaration.Property != property)
                    {
                        continue;
                    }
                    if (key.Offset <= phase)
                    {
                        before = &declaration;
                        beforeOffset = key.Offset;
                    }
                    else if (after == nullptr)
                    {
                        after = &declaration;
                        afterOffset = key.Offset;
                    }
                }
            }

            if (before == nullptr && after == nullptr)
            {
                return;
            }
            if (before == nullptr || after == nullptr)
            {
                ApplyDeclaration(live, before != nullptr ? *before : *after, assets);
                return;
            }
            if (!IsAnimatableProperty(property) || before->Unit != after->Unit)
            {
                ApplyDeclaration(live, *before, assets);
                return;
            }

            const f32 span = afterOffset - beforeOffset;
            const f32 u = span > 0.0f ? (phase - beforeOffset) / span : 1.0f;
            StyleDeclaration blended = *before;
            blended.Values = glm::mix(before->Values, after->Values, u);
            ApplyDeclaration(live, blended, assets);
        }

        // Applies one animation's keyframes at its current phase onto the live style, resolving
        // each declared property once across the whole clip.
        void ApplyAnimation(Style& live, const StyleAnimation& animation, AssetManager* assets)
        {
            const f32 phase = AnimationPhase(animation);
            vector<StyleProperty> resolved;
            for (const StyleKeyframe& key : animation.Keyframes)
            {
                for (const StyleDeclaration& declaration : key.Declarations)
                {
                    if (std::ranges::find(resolved, declaration.Property) != resolved.end())
                    {
                        continue;
                    }
                    resolved.push_back(declaration.Property);
                    ApplyAnimatedProperty(live, animation, declaration.Property, phase, assets);
                }
            }
        }
    }

    void Document::SetState(Element& element, ElementState state)
    {
        element.State = state;
        UpdateElement(element, 0.0f);
    }

    void Document::SetTransitions(Element& element, vector<StyleTransition> transitions)
    {
        element.Transitions = std::move(transitions);
        element.Tweens.clear();
    }

    void Document::UpdateElement(Element& element, f32 delta)
    {
        const Style target = ResolveTarget(element, m_Assets);

        bool layoutMoved = false;
        Style live = target;

        // A property with a positive-duration transition eases; every other property snaps to the
        // target. A tween starts when the target moves away from where the tween was heading (or from
        // the current live value when none is in flight); an animatable property whose target is
        // unchanged keeps advancing its existing tween.
        for (const StyleTransition& transition : element.Transitions)
        {
            if (transition.Duration <= 0.0f || !IsAnimatableProperty(transition.Property))
            {
                continue;
            }

            const StyleProperty property = transition.Property;
            const vec4 targetValue = ReadProperty(target, property);

            const auto existing = std::ranges::find_if(element.Tweens, [&](const StyleTween& t)
                                                       { return t.Property == property; });

            if (existing == element.Tweens.end())
            {
                const vec4 currentValue = ReadProperty(element.ComputedStyle, property);
                if (currentValue != targetValue)
                {
                    element.Tweens.push_back(StyleTween{.Property = property,
                                                        .From = currentValue,
                                                        .To = targetValue,
                                                        .Elapsed = 0.0f,
                                                        .Duration = transition.Duration});
                }
            }
            else if (existing->To != targetValue)
            {
                existing->From = ReadProperty(element.ComputedStyle, property);
                existing->To = targetValue;
                existing->Elapsed = 0.0f;
                existing->Duration = transition.Duration;
            }
        }

        // Advance and apply every in-flight tween; a length only eases within one kind, else it
        // snaps to the target (the target value already sits in `live`).
        for (auto it = element.Tweens.begin(); it != element.Tweens.end();)
        {
            StyleTween& tween = *it;
            tween.Elapsed = std::min(tween.Elapsed + delta, tween.Duration);
            const f32 t = tween.Duration > 0.0f ? tween.Elapsed / tween.Duration : 1.0f;

            // A length only eases within one kind (its ordinal rides component y); a Points→Percent
            // change snaps. Non-length payloads (colors, insets, scalars) always interpolate — but
            // a non-finite endpoint (an Unset inset edge appearing or vanishing) snaps too, since
            // mixing across the sentinel yields NaN.
            const bool kindMismatch =
                IsLengthProperty(tween.Property) && tween.From.y != tween.To.y;
            const bool nonFinite =
                glm::any(glm::isinf(tween.From)) || glm::any(glm::isinf(tween.To));
            const vec4 eased =
                kindMismatch || nonFinite ? tween.To : glm::mix(tween.From, tween.To, t);
            WriteProperty(live, tween.Property, eased);

            if (tween.Elapsed >= tween.Duration)
            {
                it = element.Tweens.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Animations write over the variant-resolved (and tweened) style at the advanced clock.
        for (StyleAnimation& animation : element.Animations)
        {
            animation.Time += delta;
            ApplyAnimation(live, animation, m_Assets);
        }

        // Detect a layout-input move against the currently-applied style before overwriting it.
        for (u32 p = 0; p < StylePropertyCount; ++p)
        {
            const auto property = static_cast<StyleProperty>(p);
            if (!IsLayoutProperty(property))
            {
                continue;
            }
            if (ReadProperty(element.ComputedStyle, property) != ReadProperty(live, property))
            {
                layoutMoved = true;
                break;
            }
        }
        // A font swap is a layout move ReadProperty does not see (a font has no numeric payload).
        const bool fontMoved =
            element.ComputedStyle.TextFont.Id().Value != live.TextFont.Id().Value;
        layoutMoved = layoutMoved || fontMoved;

        element.ComputedStyle = live;

        if (layoutMoved)
        {
            m_Dirty = true;
            // A font moves every descendant that inherits it, so the whole subtree re-measures.
            if (fontMoved)
            {
                MarkSubtreeTextDirty(element);
            }
            else if (const YGNodeRef node = m_Yoga->Get(element);
                     node != nullptr && YGNodeHasMeasureFunc(node))
            {
                YGNodeMarkDirty(node);
            }
        }
    }

    void Document::Update(f32 delta)
    {
        for (const Unique<Element>& owned : m_Elements)
        {
            UpdateElement(*owned, delta);
        }
        SyncAllScrollBars();
    }

    void Document::SyncAllScrollBars()
    {
        // A variant or transition can move the overflow, so the bars follow the resolved style
        // rather than only the authored one. Collect first: SyncScrollBars adds and removes
        // elements, which reallocates m_Elements and would strand a walk over it — the same reason
        // SyncLists collects its repeaters up front. The cached flag keeps the scan an O(1) test
        // per element, so the child walk runs only on a frame an axis actually changes.
        vector<Element*> moved;
        for (const Unique<Element>& owned : m_Elements)
        {
            // A scrollbar is never itself a scroll container, whatever style resolves onto it. The
            // exclusion is load-bearing rather than tidy: a part inherits its host's classes, so a
            // bare class rule carrying `overflow: scroll` matches the very bar its host created —
            // and without this the bar would take a bar of its own, whose parts inherit the classes
            // again, growing the tree a level per frame until the process dies. A stylesheet must
            // not be able to do that.
            if (IsScrollBarPart(owned->Kind))
            {
                continue;
            }
            if (IsScrollable(owned->ComputedStyle) != owned->Widget.HasScrollBars)
            {
                moved.push_back(owned.get());
            }
        }
        for (Element* element : moved)
        {
            SyncScrollBars(*element);
            element->Widget.HasScrollBars = IsScrollable(element->ComputedStyle);
            m_Dirty = true;
        }
    }

    bool Document::IsAnimating() const
    {
        return std::ranges::any_of(m_Elements,
                                   [](const Unique<Element>& owned)
                                   {
                                       if (!owned->Tweens.empty())
                                       {
                                           return true;
                                       }
                                       // A looping animation never settles; a play-once one settles at its last key.
                                       return std::ranges::any_of(
                                           owned->Animations,
                                           [](const StyleAnimation& animation)
                                           {
                                               return animation.Mode != AnimationLoopMode::Once ||
                                                      animation.Time < animation.Duration;
                                           });
                                   });
    }

    void Document::SetTextMeasurer(TextMeasurer measurer)
    {
        m_Measurer = std::move(measurer);
        MarkSubtreeTextDirty(*m_Root);
        m_Dirty = true;
    }

    void Document::MarkSubtreeTextDirty(const Element& element)
    {
        if (const YGNodeRef node = m_Yoga->Get(element);
            node != nullptr && YGNodeHasMeasureFunc(node))
        {
            YGNodeMarkDirty(node);
        }
        for (const Element* child : element.Children)
        {
            MarkSubtreeTextDirty(*child);
        }
    }

    const Font* Document::ResolveFont(const Element& element) const
    {
        for (const Element* cursor = &element; cursor != nullptr; cursor = cursor->Parent)
        {
            if (cursor->ComputedStyle.TextFont.IsLoaded())
            {
                return cursor->ComputedStyle.TextFont.Get();
            }
        }
        return nullptr;
    }

    vec2 Document::MeasureElementText(const Element& element, optional<f32> availableWidth) const
    {
        // A run wraps only where the style asks it to. The default is not to, so what a measure
        // reports and what DrawList::Text shapes are the same run: a label too wide for its column
        // overflows horizontally, where a clip can catch it, rather than solving a box two lines
        // tall that a single painted line then sits offset inside.
        if (element.ComputedStyle.Wrapping == TextWrap::NoWrap)
        {
            availableWidth.reset();
        }
        // A TextInput is a line box: it holds one line of its typography open even with no value,
        // so the field reserves room for the run it paints at every value, empty included.
        return MeasureRun(element.Text, ResolveFont(element), element.ComputedStyle, availableWidth,
                          element.Kind == ElementKind::TextInput);
    }

    vec2 Document::MeasureStyledText(string_view text, const Style& style,
                                     const optional<f32> availableWidth) const
    {
        return MeasureRun(text, style.TextFont.IsLoaded() ? style.TextFont.Get() : nullptr, style,
                          availableWidth, false);
    }

    vec2 Document::MeasureRun(const string_view text, const Font* const font, const Style& style,
                              const optional<f32> availableWidth, const bool emptyLineBox) const
    {
        if (m_Measurer)
        {
            return m_Measurer(text, style, availableWidth);
        }

        if (font == nullptr || (text.empty() && !emptyLineBox))
        {
            return vec2(0.0f);
        }

        const vector<u32> codepoints = DecodeUtf8(text);
        const ShapeResult shaped = font->ShapeRun(codepoints, style.TextSize, availableWidth);
        return shaped.Size;
    }

    namespace
    {
        // Whether an element kind is a focusable, interactive control — the widget layer sets
        // Element::Focusable on these so directional/Tab navigation and Confirm reach them, and a
        // plain Panel/Text/Image stays a non-stop.
        // Whether an element's direct children are addressable item slots — the containers a
        // selection can be defined over. Both repeat an authored template when they carry an
        // `items` binding and hold hand-authored children when they do not; either way each slot
        // is a whole item subtree, so an item may contain any elements at all.
        bool IsSelectionHost(ElementKind kind)
        {
            return kind == ElementKind::List || kind == ElementKind::Table;
        }

        // Maps a markup `selection` attribute value to its mode; an unrecognized value is None.
        SelectionMode ParseSelectionMode(string_view value)
        {
            if (value == "single")
            {
                return SelectionMode::Single;
            }
            if (value == "multiple")
            {
                return SelectionMode::Multiple;
            }
            if (value == "extended")
            {
                return SelectionMode::Extended;
            }
            return SelectionMode::None;
        }

        bool IsFocusableWidget(ElementKind kind)
        {
            switch (kind)
            {
            case ElementKind::Button:
            case ElementKind::Checkbox:
            case ElementKind::Slider:
            case ElementKind::TextInput:
            case ElementKind::ScrollView:
                return true;
            case ElementKind::Panel:
            case ElementKind::Text:
            case ElementKind::Image:
            case ElementKind::ProgressBar:
            case ElementKind::List:
            case ElementKind::Table:
            case ElementKind::ScrollBar:
            case ElementKind::ScrollBarThumb:
            case ElementKind::SliderFill:
            case ElementKind::SliderThumb:
                return false;
            }
            return false;
        }

        // Clamps `value` to [min, max] and snaps it to the nearest `step` multiple above min when the
        // step is positive — the Slider/Checkbox value normalization.
        f32 ClampStep(f32 value, f32 min, f32 max, f32 step)
        {
            const f32 lo = std::min(min, max);
            const f32 hi = std::max(min, max);
            f32 clamped = std::clamp(value, lo, hi);
            if (step > 0.0f)
            {
                clamped = lo + std::round((clamped - lo) / step) * step;
                clamped = std::clamp(clamped, lo, hi);
            }
            return clamped;
        }

        // Reads a widget-config attribute a control carries in its Bindings map as a literal number,
        // returning the fallback when it is absent or unparseable. Uses from_chars — the engine
        // builds -fno-exceptions, so the throwing stof/stod are off-limits.
        f32 ReadConfigScalar(const Element& element, string_view name, f32 fallback)
        {
            const auto it = element.Bindings.find(string{name});
            if (it == element.Bindings.end())
            {
                return fallback;
            }
            const string& text = it->second;
            f32 value = fallback;
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (ec != std::errc{})
            {
                return fallback;
            }
            return value;
        }
    }

    void Document::SetWidgetValue(Element& element, f32 value)
    {
        if (element.Kind != ElementKind::Slider && element.Kind != ElementKind::Checkbox &&
            element.Kind != ElementKind::ProgressBar)
        {
            return;
        }

        const f32 clamped =
            element.Kind == ElementKind::Checkbox
                ? (value != 0.0f ? 1.0f : 0.0f)
                : ClampStep(value, element.Widget.Min, element.Widget.Max, element.Widget.Step);
        const bool changed = clamped != element.Widget.Value;
        element.Widget.Value = clamped;

        // A Checkbox reflects its value into the Checked state bit so the `:checked` variant resolves.
        if (element.Kind == ElementKind::Checkbox)
        {
            SetState(element, WithBit(element.State, ElementState::Checked, clamped != 0.0f));
        }

        // The fill and thumb are placed from the value against the host's already-solved box, so a
        // value change re-places them directly rather than dirtying the tree — dragging a slider
        // must not re-run the flex solve every pointer move.
        if (element.Kind == ElementKind::Slider)
        {
            LayoutSliderParts(element);
        }

        if (changed && element.Kind != ElementKind::ProgressBar)
        {
            static_cast<void>(FireHandler(element, "onChange"));
        }
    }

    void Document::ScrollBy(Element& element, vec2 delta)
    {
        const Style& style = element.ComputedStyle;
        if (!IsScrollable(style))
        {
            return;
        }

        // The scrollable extent is how far the content overflows the viewport; clamp so the offset
        // never scrolls the content past its own bounds. The widget-owned scrollbars are pinned to
        // the element's edges rather than carried by the content, so they never extend it.
        const vec2 overflow = ScrollRange(element);

        const vec2 next = glm::clamp(element.Widget.ScrollOffset + delta, vec2(0.0f), overflow);
        if (next != element.Widget.ScrollOffset)
        {
            element.Widget.ScrollOffset = next;
            m_Dirty = true;
        }
    }

    void Document::CascadeWidgetElement(Element& element)
    {
        vector<const StyleSheet*> sheets;
        sheets.reserve(m_StyleSheets.size());
        for (const AssetHandle<StyleSheet>& handle : m_StyleSheets)
        {
            if (handle.IsLoaded())
            {
                sheets.push_back(handle.Get());
            }
        }
        if (sheets.empty())
        {
            return;
        }

        AssetManager* const assets = m_Assets;
        auto& cache = m_GradientCache;
        const function<optional<ResolvedGradient>(const StyleSheet&, u32)> resolveGradient =
            [assets, &cache](const StyleSheet& sheet, const u32 index) -> optional<ResolvedGradient>
        {
            const vector<StyleGradient>& gradients = sheet.GetGradients();
            if (index >= gradients.size() || assets == nullptr)
            {
                return std::nullopt;
            }
            const StyleGradient& source = gradients[index];
            if (const auto it = cache.find(&source); it != cache.end())
            {
                return it->second;
            }
            const TextureData data{
                .Name = "gui-gradient-ramp",
                .Extent = uvec2(source.Width, 1),
                .Format = Renderer::Format::RGBA16Sfloat,
                .MipLevels = 1,
                .Pixels = source.Ramp,
                .Sampler =
                    Renderer::SamplerInfo{.AddressModeU = Renderer::AddressMode::ClampToEdge,
                                          .AddressModeV = Renderer::AddressMode::ClampToEdge,
                                          .AddressModeW = Renderer::AddressMode::ClampToEdge},
            };
            const ResolvedGradient resolved{.Kind = source.Kind,
                                            .P0 = source.P0,
                                            .P1 = source.P1,
                                            .AngleOffset = source.AngleOffset,
                                            .Ramp = assets->BuildSync<Texture>(data)};
            cache.emplace(&source, resolved);
            return resolved;
        };

        ResolveElementStyle(element, nullptr, sheets, m_Assets, resolveGradient);
    }

    Element& Document::CreateWidgetPart(Element& host, const ElementKind kind,
                                        const string_view tag)
    {
        Element& part = Add(host, kind);

        // A part inherits its host's classes, then adds its own tag. The selector engine matches a
        // single compound selector with no descendant combinator, so this is what makes a part
        // addressable per instance: `ScrollBar.track-list` or `SliderThumb.slice-slider` reaches
        // one host's parts, where `.track-list ScrollBar` would not parse.
        part.Classes = host.Classes;
        if (!tag.empty())
        {
            part.Classes.emplace_back(tag);
        }

        // A part is placed by the widget layer against the host's solved box, never by the flex
        // flow, so it is absolute both before and after the cascade — a rule may style a part's
        // paint but not take it back into flow.
        part.BaseStyle.Position = PositionType::Absolute;
        CascadeWidgetElement(part);
        part.BaseStyle.Position = PositionType::Absolute;
        part.ComputedStyle.Position = PositionType::Absolute;
        return part;
    }

    Element* Document::FindPart(const Element& element, const ElementKind kind) const
    {
        for (Element* child : element.Children)
        {
            if (child->Kind == kind)
            {
                return child;
            }
        }
        return nullptr;
    }

    void Document::SyncSliderParts(Element& element)
    {
        if (element.Kind != ElementKind::Slider || FindPart(element, ElementKind::SliderFill))
        {
            return;
        }

        // The Slider element itself is the track; the fill and thumb are parts over it, so a
        // slider's own background/border stay its own rather than doubling as the handle's color.
        Element& fill = CreateWidgetPart(element, ElementKind::SliderFill, {});
        if (fill.BaseStyle.Background.a == 0.0f)
        {
            fill.BaseStyle.Background = DefaultSliderFill;
            fill.ComputedStyle.Background = DefaultSliderFill;
        }
        Element& thumb = CreateWidgetPart(element, ElementKind::SliderThumb, {});
        if (thumb.BaseStyle.Background.a == 0.0f)
        {
            thumb.BaseStyle.Background = DefaultSliderThumb;
            thumb.ComputedStyle.Background = DefaultSliderThumb;
        }
    }

    void Document::LayoutSliderParts(Element& element)
    {
        Element* const fill = FindPart(element, ElementKind::SliderFill);
        Element* const thumb = FindPart(element, ElementKind::SliderThumb);
        if (fill == nullptr || thumb == nullptr)
        {
            return;
        }

        const Rect& box = element.Layout;
        const f32 range = element.Widget.Max - element.Widget.Min;
        const f32 fraction =
            range != 0.0f
                ? std::clamp((element.Widget.Value - element.Widget.Min) / range, 0.0f, 1.0f)
                : 0.0f;

        if (element.Widget.Vertical)
        {
            // A vertical slider fills from the bottom (Min) toward the top (Max); the square thumb
            // spans the track's width and rides the fill's top edge.
            const f32 filled = box.Size.y * fraction;
            fill->Layout = Rect{.Min = vec2(box.Min.x, box.Min.y + box.Size.y - filled),
                                .Size = vec2(box.Size.x, filled)};
            const f32 size = box.Size.x;
            thumb->Layout =
                Rect{.Min = vec2(box.Min.x,
                                 box.Min.y + (1.0f - fraction) * std::max(box.Size.y - size, 0.0f)),
                     .Size = vec2(size, size)};
        }
        else
        {
            fill->Layout = Rect{.Min = box.Min, .Size = vec2(box.Size.x * fraction, box.Size.y)};
            const f32 size = box.Size.y;
            thumb->Layout = Rect{
                .Min = vec2(box.Min.x + fraction * std::max(box.Size.x - size, 0.0f), box.Min.y),
                .Size = vec2(size, size)};
        }
        fill->Visible = fraction > 0.0f;
    }

    Element* Document::FindScrollBar(const Element& element, const bool vertical) const
    {
        for (Element* child : element.Children)
        {
            if (child->Kind == ElementKind::ScrollBar && child->Widget.Vertical == vertical)
            {
                return child;
            }
        }
        return nullptr;
    }

    f32 Document::ScrollBarThickness(const Element& element) const
    {
        // A bar's thickness is its own styled cross-axis length, so `ScrollBar { width: 6px }`
        // narrows both the bar and — under a gutter — the space reserved for it, from one value.
        const Length& length =
            element.Widget.Vertical ? element.ComputedStyle.Width : element.ComputedStyle.Height;
        if (length.Kind == LengthKind::Points && length.Value > 0.0f)
        {
            return length.Value;
        }
        return DefaultScrollBarThickness;
    }

    void Document::SyncScrollBars(Element& element)
    {
        const Style& style = element.ComputedStyle;
        for (const bool vertical : {false, true})
        {
            const bool wanted = ScrollsAxis(vertical ? style.OverflowY : style.OverflowX);
            Element* bar = FindScrollBar(element, vertical);
            if (wanted == (bar != nullptr))
            {
                continue;
            }
            if (!wanted)
            {
                Remove(*bar);
                continue;
            }

            // The bar and its thumb are appended after the content and take no part in the flex
            // flow — LayoutScrollBars writes their rects directly against the solved box.
            const string_view axis = vertical ? "vertical" : "horizontal";
            Element& created = CreateWidgetPart(element, ElementKind::ScrollBar, axis);
            created.Widget.Vertical = vertical;
            Element& thumb = CreateWidgetPart(created, ElementKind::ScrollBarThumb, axis);
            thumb.Widget.Vertical = vertical;
        }
    }

    Insets Document::ContentPadding(const Element& element) const
    {
        // A gutter reserves each scrollable axis's bar thickness out of the content box, as extra
        // padding on the edge the bar sits against — so the content never flows under the bar. The
        // space is held whether or not the axis currently overflows, which is what keeps the
        // content from shifting the moment it grows past the box. An overlay reserves nothing.
        const Style& style = element.ComputedStyle;
        Insets padding = style.Padding;
        if (style.Scrollbar == ScrollbarLayout::Gutter && IsScrollable(style))
        {
            if (const Element* const bar = FindScrollBar(element, true); bar != nullptr)
            {
                padding.Right += ScrollBarThickness(*bar);
            }
            if (const Element* const bar = FindScrollBar(element, false); bar != nullptr)
            {
                padding.Bottom += ScrollBarThickness(*bar);
            }
        }
        return padding;
    }

    vec2 Document::ScrollRange(const Element& element) const
    {
        const Style& style = element.ComputedStyle;

        // A scrollable element's children are laid out shifted by its scroll offset, so their solved
        // boxes measure the content *from where it sat when they were read*. The shift has to come
        // back out, or the range shrinks by exactly as much as the content has already scrolled —
        // which leaves the last of it permanently out of reach, and past the halfway point clamps
        // the offset back down to a value it has already passed.
        const vec2 scrolled = element.Widget.LayoutScrollOffset;
        vec2 content(0.0f);
        for (const Element* child : ContentChildren(element))
        {
            content = glm::max(content, child->Layout.Max() + scrolled - element.Layout.Min);
        }

        // The scrollable region runs to the container's own far content edge — its padding, its
        // reserved gutter and its border — so the last child clears the inside of the box the way
        // the first one does. Without it the content's end is flush against the frame and the
        // element's padding reads as applying only at the top.
        const f32 border = BorderWidth(style);
        const Insets padding = ContentPadding(element);
        content += vec2(padding.Right + border, padding.Bottom + border);

        vec2 range = glm::max(content - element.Layout.Size, vec2(0.0f));
        range.x = ScrollsAxis(style.OverflowX) ? range.x : 0.0f;
        range.y = ScrollsAxis(style.OverflowY) ? range.y : 0.0f;
        return range;
    }

    void Document::LayoutScrollBars(Element& element)
    {
        const vec2 range = ScrollRange(element);
        const Rect& box = element.Layout;

        for (const bool vertical : {false, true})
        {
            Element* const bar = FindScrollBar(element, vertical);
            if (bar == nullptr)
            {
                continue;
            }

            // An axis with no travel hides its bar rather than dropping it: presence is decided by
            // the style, visibility by whether there is anything to scroll, so content growing past
            // the box reveals the bar with no structural change.
            const f32 travel = vertical ? range.y : range.x;
            bar->Visible = travel > 0.0f;
            if (!bar->Visible)
            {
                continue;
            }

            const f32 thickness = ScrollBarThickness(*bar);
            // When both axes scroll, each bar stops short of the other's corner so they do not
            // overlap at the inner corner.
            const Element* const other = FindScrollBar(element, !vertical);
            const f32 inset =
                other != nullptr && other->Visible ? ScrollBarThickness(*other) : 0.0f;

            bar->Layout = vertical ? Rect{.Min = vec2(box.Max().x - thickness, box.Min.y),
                                          .Size = vec2(thickness, box.Size.y - inset)}
                                   : Rect{.Min = vec2(box.Min.x, box.Max().y - thickness),
                                          .Size = vec2(box.Size.x - inset, thickness)};

            if (bar->Children.empty())
            {
                continue;
            }
            Element& thumb = *bar->Children.front();
            thumb.Visible = true;

            // The thumb's length is the visible fraction of the content, floored so a very long
            // list still leaves something grabbable; its travel maps the scroll offset onto the
            // slack left in the track.
            const f32 track = vertical ? bar->Layout.Size.y : bar->Layout.Size.x;
            const f32 viewport = vertical ? box.Size.y : box.Size.x;
            const f32 content = viewport + travel;
            const f32 length = content > 0.0f
                                   ? std::clamp(track * (viewport / content),
                                                std::min(MinScrollThumbLength, track), track)
                                   : track;
            const f32 offset =
                vertical ? element.Widget.ScrollOffset.y : element.Widget.ScrollOffset.x;
            const f32 slide = travel > 0.0f ? (offset / travel) * (track - length) : 0.0f;

            thumb.Layout = vertical
                               ? Rect{.Min = vec2(bar->Layout.Min.x, bar->Layout.Min.y + slide),
                                      .Size = vec2(bar->Layout.Size.x, length)}
                               : Rect{.Min = vec2(bar->Layout.Min.x + slide, bar->Layout.Min.y),
                                      .Size = vec2(length, bar->Layout.Size.y)};
        }
    }

    void Document::ApplyWidgetFocusability(Element& element)
    {
        if (IsFocusableWidget(element.Kind))
        {
            element.Focusable = true;
        }
    }

    void Document::InitWidget(Element& element)
    {
        ApplyWidgetFocusability(element);
        SyncScrollBars(element);
        SyncSliderParts(element);
        element.Widget.HasScrollBars = IsScrollable(element.ComputedStyle);

        if (element.Kind == ElementKind::Slider)
        {
            element.Widget.Min = ReadConfigScalar(element, "min", 0.0f);
            element.Widget.Max = ReadConfigScalar(element, "max", 1.0f);
            element.Widget.Step = ReadConfigScalar(element, "step", 0.0f);
            element.Widget.Value =
                ClampStep(ReadConfigScalar(element, "value", element.Widget.Min),
                          element.Widget.Min, element.Widget.Max, element.Widget.Step);
            const auto orientation = element.Bindings.find("orientation");
            element.Widget.Vertical =
                orientation != element.Bindings.end() && orientation->second == "vertical";
        }
        else if (element.Kind == ElementKind::ProgressBar)
        {
            element.Widget.Min = 0.0f;
            element.Widget.Max = 1.0f;
            element.Widget.Value = std::clamp(ReadConfigScalar(element, "value", 0.0f), 0.0f, 1.0f);
        }
        else if (element.Kind == ElementKind::Checkbox)
        {
            const bool checked = ReadConfigScalar(element, "value", 0.0f) != 0.0f ||
                                 (element.Bindings.count("checked") != 0 &&
                                  element.Bindings.at("checked") == "true");
            element.Widget.Value = checked ? 1.0f : 0.0f;
            if (checked)
            {
                element.State = element.State | ElementState::Checked;
            }
        }
        else if (element.Kind == ElementKind::TextInput)
        {
            element.Widget.Caret = static_cast<u32>(DecodeUtf8(element.Text).size());
        }
        else if (IsSelectionHost(element.Kind))
        {
            const auto selection = element.Bindings.find("selection");
            element.Widget.Selection = selection == element.Bindings.end()
                                           ? SelectionMode::None
                                           : ParseSelectionMode(selection->second);
        }
    }

    bool Document::DriveWidgetPointer(Element& element, const PointerEvent& event)
    {
        if (element.Kind == ElementKind::Slider)
        {
            // A press or drag over the track sets the value from the pointer's fraction along the
            // slider's run — Min at the left edge to Max at the right for a horizontal slider, Min
            // at the bottom edge to Max at the top for a vertical one (document y grows downward,
            // so the vertical fraction inverts).
            if (event.Kind != PointerEventKind::Down && event.Kind != PointerEventKind::Move)
            {
                return false;
            }
            if (event.Kind == PointerEventKind::Move && m_PressTarget != &element)
            {
                return false;
            }
            const f32 run = element.Widget.Vertical ? element.Layout.Size.y : element.Layout.Size.x;
            if (run <= 0.0f)
            {
                return false;
            }
            const f32 fraction =
                element.Widget.Vertical
                    ? std::clamp(1.0f - (event.Position.y - element.Layout.Min.y) / run, 0.0f, 1.0f)
                    : std::clamp((event.Position.x - element.Layout.Min.x) / run, 0.0f, 1.0f);
            SetWidgetValue(element, element.Widget.Min +
                                        fraction * (element.Widget.Max - element.Widget.Min));
            return true;
        }

        if (element.Kind == ElementKind::ScrollBarThumb)
        {
            // Dragging the thumb moves the content by the pointer delta scaled through the track:
            // the thumb crosses the track's slack while the content crosses its whole range, so a
            // short track drags a long list proportionally.
            if (event.Kind != PointerEventKind::Move || m_PressTarget != &element)
            {
                return false;
            }
            const Element* const bar = element.Parent;
            Element* const view = bar != nullptr ? bar->Parent : nullptr;
            if (view == nullptr)
            {
                return false;
            }
            const bool vertical = element.Widget.Vertical;
            const f32 track = vertical ? bar->Layout.Size.y : bar->Layout.Size.x;
            const f32 length = vertical ? element.Layout.Size.y : element.Layout.Size.x;
            const f32 slack = track - length;
            const vec2 moved = event.Position - m_LastScrollPointer;
            m_LastScrollPointer = event.Position;
            if (slack <= 0.0f)
            {
                return true;
            }
            const f32 travel = vertical ? ScrollRange(*view).y : ScrollRange(*view).x;
            const f32 delta = (vertical ? moved.y : moved.x) / slack * travel;
            ScrollBy(*view, vertical ? vec2(0.0f, delta) : vec2(delta, 0.0f));
            return true;
        }

        if (element.Kind == ElementKind::ScrollBar)
        {
            // A press on the track pages one viewport toward the pointer — the thumb itself hits
            // first, so reaching the bar means the pointer landed beside it.
            if (event.Kind != PointerEventKind::Down || element.Children.empty())
            {
                return false;
            }
            Element* const view = element.Parent;
            if (view == nullptr)
            {
                return false;
            }
            const bool vertical = element.Widget.Vertical;
            const Rect& thumb = element.Children.front()->Layout;
            const f32 pointer = vertical ? event.Position.y : event.Position.x;
            const f32 near = vertical ? thumb.Min.y : thumb.Min.x;
            const f32 far = vertical ? thumb.Max().y : thumb.Max().x;
            const f32 page = vertical ? view->Layout.Size.y : view->Layout.Size.x;
            const f32 step = pointer < near ? -page : (pointer > far ? page : 0.0f);
            ScrollBy(*view, vertical ? vec2(0.0f, step) : vec2(step, 0.0f));
            return true;
        }

        if (IsScrollable(element.ComputedStyle))
        {
            // A drag with the press captured on the scrollable element pans its content by the
            // pointer delta.
            if (event.Kind != PointerEventKind::Move || m_PressTarget != &element)
            {
                return false;
            }
            const vec2 delta = m_LastScrollPointer - event.Position;
            m_LastScrollPointer = event.Position;
            const vec2 before = element.Widget.ScrollOffset;
            ScrollBy(element, delta);
            // The content moved, so this press is a scroll and not a click on whatever it started
            // over — the release reads this to decline the click it would otherwise complete.
            m_PressPanned = m_PressPanned || element.Widget.ScrollOffset != before;
            return true;
        }

        return false;
    }

    bool Document::DriveWidgetNavigation(Element& element, NavAction action)
    {
        if (element.Kind == ElementKind::Slider)
        {
            const f32 step = element.Widget.Step > 0.0f
                                 ? element.Widget.Step
                                 : (element.Widget.Max - element.Widget.Min) * 0.05f;
            // A horizontal slider nudges on left/right; a vertical one on up/down (up is +, the
            // physical-slider convention matching its bottom-Min paint and pointer mapping).
            const NavAction decrease =
                element.Widget.Vertical ? NavAction::MoveDown : NavAction::MoveLeft;
            const NavAction increase =
                element.Widget.Vertical ? NavAction::MoveUp : NavAction::MoveRight;
            if (action == decrease)
            {
                SetWidgetValue(element, element.Widget.Value - step);
                return true;
            }
            if (action == increase)
            {
                SetWidgetValue(element, element.Widget.Value + step);
                return true;
            }
            return false;
        }

        if (IsScrollable(element.ComputedStyle))
        {
            const f32 line =
                element.ComputedStyle.TextSize > 0.0f ? element.ComputedStyle.TextSize : 16.0f;
            switch (action)
            {
            case NavAction::MoveUp:
                ScrollBy(element, vec2(0.0f, -line));
                return true;
            case NavAction::MoveDown:
                ScrollBy(element, vec2(0.0f, line));
                return true;
            default:
                return false;
            }
        }

        return false;
    }

    bool Document::DriveWidgetText(Element& element, u32 codepoint)
    {
        if (element.Kind != ElementKind::TextInput)
        {
            return false;
        }

        // The caret indexes codepoints; decode, edit, and re-encode so a multi-byte glyph is one edit
        // unit. A backspace (U+0008) deletes the codepoint before the caret; U+007F (delete) the one
        // after; every other codepoint inserts at the caret.
        vector<u32> codepoints = DecodeUtf8(element.Text);
        u32 caret = std::min(element.Widget.Caret, static_cast<u32>(codepoints.size()));

        if (codepoint == 0x08)
        {
            if (caret == 0)
            {
                return false;
            }
            codepoints.erase(codepoints.begin() + (caret - 1));
            --caret;
        }
        else if (codepoint == 0x7F)
        {
            if (caret >= codepoints.size())
            {
                return false;
            }
            codepoints.erase(codepoints.begin() + caret);
        }
        else if (codepoint >= 0x20)
        {
            codepoints.insert(codepoints.begin() + caret, codepoint);
            ++caret;
        }
        else
        {
            return false;
        }

        string edited;
        for (const u32 cp : codepoints)
        {
            AppendUtf8(edited, cp);
        }
        element.Widget.Caret = caret;
        SetText(element, edited);
        static_cast<void>(FireHandler(element, "onChange"));
        return true;
    }

    bool Document::DriveWidgetTextEdit(Element& element, TextEditAction action)
    {
        if (element.Kind != ElementKind::TextInput)
        {
            return false;
        }

        // A delete is the same edit a typed control character performs, so both routes share one
        // implementation and one onChange.
        if (action == TextEditAction::DeleteBackward)
        {
            return DriveWidgetText(element, 0x08);
        }
        if (action == TextEditAction::DeleteForward)
        {
            return DriveWidgetText(element, 0x7F);
        }

        // The caret indexes codepoints, so a move steps one decoded codepoint — never into the
        // middle of a multi-byte glyph — and clamps at both ends of the value. A clamped move still
        // consumes the action: the field owns its caret keys whether or not the caret can travel,
        // so an arrow at either end does not leak out and move focus.
        const auto length = static_cast<u32>(DecodeUtf8(element.Text).size());
        const u32 caret = std::min(element.Widget.Caret, length);

        switch (action)
        {
        case TextEditAction::CaretLeft:
            element.Widget.Caret = caret > 0 ? caret - 1 : 0;
            break;
        case TextEditAction::CaretRight:
            element.Widget.Caret = caret < length ? caret + 1 : length;
            break;
        case TextEditAction::CaretHome:
            element.Widget.Caret = 0;
            break;
        case TextEditAction::CaretEnd:
            element.Widget.Caret = length;
            break;
        default:
            break;
        }
        return true;
    }

    void Document::ApplyStyle(Element& element)
    {
        const YGNodeRef node = m_Yoga->Get(element);
        if (node == nullptr)
        {
            return;
        }

        const Style& style = element.ComputedStyle;

        YGNodeStyleSetFlexDirection(node, ToYogaDirection(style.Direction));
        YGNodeStyleSetJustifyContent(node, ToYogaJustify(style.JustifyContent));
        YGNodeStyleSetAlignItems(node, ToYogaAlign(style.AlignItems));
        YGNodeStyleSetAlignSelf(node, ToYogaAlign(style.AlignSelf));
        YGNodeStyleSetFlexWrap(node, ToYogaWrap(style.Wrap));

        YGNodeStyleSetFlexGrow(node, style.FlexGrow);
        YGNodeStyleSetFlexShrink(node, style.FlexShrink);
        ApplyLength(
            style.FlexBasis, [&](f32 v) { YGNodeStyleSetFlexBasis(node, v); },
            [&](f32 v) { YGNodeStyleSetFlexBasisPercent(node, v); },
            [&] { YGNodeStyleSetFlexBasisAuto(node); });

        ApplyLength(
            style.Width, [&](f32 v) { YGNodeStyleSetWidth(node, v); }, [&](f32 v)
            { YGNodeStyleSetWidthPercent(node, v); }, [&] { YGNodeStyleSetWidthAuto(node); });
        ApplyLength(
            style.Height, [&](f32 v) { YGNodeStyleSetHeight(node, v); }, [&](f32 v)
            { YGNodeStyleSetHeightPercent(node, v); }, [&] { YGNodeStyleSetHeightAuto(node); });

        ApplyLength(
            style.MinWidth, [&](f32 v) { YGNodeStyleSetMinWidth(node, v); },
            [&](f32 v) { YGNodeStyleSetMinWidthPercent(node, v); },
            [&] { YGNodeStyleSetMinWidth(node, YGUndefined); });
        ApplyLength(
            style.MinHeight, [&](f32 v) { YGNodeStyleSetMinHeight(node, v); },
            [&](f32 v) { YGNodeStyleSetMinHeightPercent(node, v); },
            [&] { YGNodeStyleSetMinHeight(node, YGUndefined); });
        ApplyLength(
            style.MaxWidth, [&](f32 v) { YGNodeStyleSetMaxWidth(node, v); },
            [&](f32 v) { YGNodeStyleSetMaxWidthPercent(node, v); },
            [&] { YGNodeStyleSetMaxWidth(node, YGUndefined); });
        ApplyLength(
            style.MaxHeight, [&](f32 v) { YGNodeStyleSetMaxHeight(node, v); },
            [&](f32 v) { YGNodeStyleSetMaxHeightPercent(node, v); },
            [&] { YGNodeStyleSetMaxHeight(node, YGUndefined); });

        ApplyEdgeInsets(node, style.Margin, &YGNodeStyleSetMargin);

        // The border edge sits between margin and padding, so the frame is reserved out of the
        // content box: children land inside it, a measure function is handed the box its content is
        // actually drawn in, and an auto-sized leaf includes its own frame. The element's rect stays
        // the border box, which is what every paint site deflates from.
        ApplyEdgeInsets(node, Insets::All(BorderWidth(style)), &YGNodeStyleSetBorder);

        ApplyEdgeInsets(node, ContentPadding(element), &YGNodeStyleSetPadding);

        YGNodeStyleSetOverflow(node, IsScrollable(style)   ? YGOverflowScroll
                                     : ClipsContent(style) ? YGOverflowHidden
                                                           : YGOverflowVisible);

        YGNodeStyleSetPositionType(node, style.Position == PositionType::Absolute
                                             ? YGPositionTypeAbsolute
                                             : YGPositionTypeRelative);
        if (style.Position == PositionType::Absolute)
        {
            // Only set edges constrain; an Unset edge pushes YGUndefined so an anchored element
            // keeps its own (styled or content) size instead of stretching between zero insets.
            const auto applyEdge = [&](YGEdge edge, f32 value)
            {
                YGNodeStyleSetPosition(node, edge,
                                       PositionInsets::IsSet(value) ? value : YGUndefined);
            };
            applyEdge(YGEdgeLeft, style.Inset.Left);
            applyEdge(YGEdgeTop, style.Inset.Top);
            applyEdge(YGEdgeRight, style.Inset.Right);
            applyEdge(YGEdgeBottom, style.Inset.Bottom);
        }

        // A hidden subtree is removed from layout: its node measures as a zero box.
        YGNodeStyleSetDisplay(node, element.Visible ? YGDisplayFlex : YGDisplayNone);

        for (Element* child : element.Children)
        {
            ApplyStyle(*child);
        }
    }

    void Document::ReadLayout(Element& element, vec2 origin)
    {
        const YGNodeRef node = m_Yoga->Get(element);
        if (node == nullptr)
        {
            return;
        }

        const vec2 localMin(YGNodeLayoutGetLeft(node), YGNodeLayoutGetTop(node));
        const vec2 size(YGNodeLayoutGetWidth(node), YGNodeLayoutGetHeight(node));
        // The self-anchor: shift by -Origin · size so the solved position names the anchor point
        // rather than the top-left. Children recurse from the shifted min, so the subtree rides.
        const vec2 absoluteMin = origin + localMin - element.ComputedStyle.Origin * size;
        element.Layout = Rect{
            .Min = absoluteMin,
            .Size = size,
        };

        // A scrollable element shifts its content by its scroll offset, so the child origin is its
        // top-left minus the offset — the content slides under the clip it paints with. The shift
        // is recorded beside it, because the rects the children are about to take are the only
        // measure of the scrollable extent there is, and reading that extent later means undoing
        // the shift they were actually read with.
        const bool scrolls = IsScrollable(element.ComputedStyle);
        element.Widget.LayoutScrollOffset = scrolls ? element.Widget.ScrollOffset : vec2(0.0f);
        const vec2 childOrigin = scrolls ? absoluteMin - element.Widget.ScrollOffset : absoluteMin;

        for (Element* child : element.Children)
        {
            ReadLayout(*child, childOrigin);
        }

        // The bars are pinned to the element's own box, so they are placed after the content is
        // read and are unaffected by the scroll shift the content rode in on.
        if (IsScrollable(element.ComputedStyle))
        {
            LayoutScrollBars(element);
        }
        if (element.Kind == ElementKind::Slider)
        {
            LayoutSliderParts(element);
        }
    }

    void Document::Solve(vec2 available)
    {
        if (!m_Dirty && available == m_LastAvailable)
        {
            return;
        }

        ApplyStyle(*m_Root);

        const YGNodeRef rootNode = m_Yoga->Get(*m_Root);
        YGNodeCalculateLayout(rootNode, available.x, available.y, YGDirectionLTR);

        // A Table's cells widen to their per-column maxima measured off the first pass; a raised
        // min-width re-runs the layout once. ApplyStyle re-pushes the styled min-widths on every
        // Solve, so the natural (un-widened) widths above are what the columns are measured from.
        if (AlignTableColumns())
        {
            YGNodeCalculateLayout(rootNode, available.x, available.y, YGDirectionLTR);
        }

        ReadLayout(*m_Root, vec2(0.0f));

        // Popups lay out against the document extent, not against a parent box, and their
        // placement reads their anchor's solved rect — so they are solved after the main tree's
        // layout read, bottom-up (a submenu anchored inside its menu reads a placed anchor).
        for (const Popup& popup : m_Popups)
        {
            SolvePopup(popup, available);
        }

        m_Dirty = false;
        m_LastAvailable = available;
    }

    void Document::SolvePopup(const Popup& popup, const vec2 available)
    {
        const YGNodeRef node = m_Yoga->Get(*popup.Root);
        if (node == nullptr || popup.AnchorElement == nullptr)
        {
            return;
        }

        ApplyStyle(*popup.Root);
        // Unconstrained on both axes, so the root sizes to its content unless its own style bounds
        // it — a menu is as wide as its widest item and as tall as its items, with `max-height`
        // (plus `overflow-y: scroll`) the way an author caps a long one.
        YGNodeCalculateLayout(node, YGUndefined, YGUndefined, YGDirectionLTR);

        const vec2 size(YGNodeLayoutGetWidth(node), YGNodeLayoutGetHeight(node));
        const Rect& anchor = popup.AnchorElement->Layout;
        vec2 corner{0.0f};
        switch (popup.Options.Side)
        {
        case PopupSide::Below:
            corner = vec2(anchor.Min.x, anchor.Max().y);
            break;
        case PopupSide::Above:
            corner = vec2(anchor.Min.x, anchor.Min.y - size.y);
            break;
        case PopupSide::RightOf:
            corner = vec2(anchor.Max().x, anchor.Min.y);
            break;
        case PopupSide::LeftOf:
            corner = vec2(anchor.Min.x - size.x, anchor.Min.y);
            break;
        }

        ReadLayout(*popup.Root, AnchorBeside(corner, size, popup.Options.Offset, available,
                                             popup.Options.Margin));
    }

    bool Document::AlignTableColumns()
    {
        bool changed = false;
        for (const Unique<Element>& owned : m_Elements)
        {
            if (owned->Kind != ElementKind::Table || !owned->Visible)
            {
                continue;
            }

            // A row is an in-flow, visible direct child; a cell is the same one level down. An
            // absolutely-positioned child sits outside the flow (an overlay, a rule), so it
            // neither contributes to nor receives a column width. A growing cell is an elastic
            // filler, not a column: its first-pass width is its own row's slack (different per
            // row), so measuring it would poison the column maximum and pinning it would defeat
            // the grow — it keeps its column index but is otherwise left alone, absorbing
            // per-row width differences so the fixed columns after it stay right-anchored.
            const auto inFlow = [](const Element& element)
            { return element.Visible && element.ComputedStyle.Position != PositionType::Absolute; };
            const auto isFiller = [](const Element& element)
            { return element.ComputedStyle.FlexGrow > 0.0f; };

            // Column k's width is the widest k-th cell margin box across the table's rows.
            vector<f32> columns;
            for (const Element* row : ContentChildren(*owned))
            {
                if (!inFlow(*row))
                {
                    continue;
                }
                usize index = 0;
                for (const Element* cell : row->Children)
                {
                    if (!inFlow(*cell))
                    {
                        continue;
                    }
                    const YGNodeRef node = m_Yoga->Get(*cell);
                    if (node == nullptr || isFiller(*cell))
                    {
                        ++index;
                        continue;
                    }
                    const f32 width = YGNodeLayoutGetWidth(node) +
                                      YGNodeLayoutGetMargin(node, YGEdgeLeft) +
                                      YGNodeLayoutGetMargin(node, YGEdgeRight);
                    if (index >= columns.size())
                    {
                        columns.resize(index + 1, 0.0f);
                    }
                    columns[index] = std::max(columns[index], width);
                    ++index;
                }
            }

            // Raise each cell's min-width to its column's width less its own margins. The styled
            // min-width was pushed by ApplyStyle, so only a genuinely wider column moves a node.
            for (const Element* row : ContentChildren(*owned))
            {
                if (!inFlow(*row))
                {
                    continue;
                }
                usize index = 0;
                for (const Element* cell : row->Children)
                {
                    if (!inFlow(*cell))
                    {
                        continue;
                    }
                    const YGNodeRef node = m_Yoga->Get(*cell);
                    if (node == nullptr || isFiller(*cell) || index >= columns.size())
                    {
                        ++index;
                        continue;
                    }
                    const f32 target = columns[index] - YGNodeLayoutGetMargin(node, YGEdgeLeft) -
                                       YGNodeLayoutGetMargin(node, YGEdgeRight);
                    const YGValue current = YGNodeStyleGetMinWidth(node);
                    if (target > 0.0f && (current.unit != YGUnitPoint || current.value < target))
                    {
                        YGNodeStyleSetMinWidth(node, target);
                        changed = true;
                    }
                    ++index;
                }
            }
        }
        return changed;
    }

    void Document::BuildElement(const Element& element, DrawList& list, const f32 inherited) const
    {
        if (!element.Visible)
        {
            return;
        }

        const Style& style = element.ComputedStyle;
        const Rect& rect = element.Layout;

        // The style opacity composites over the subtree: every primitive this element and its
        // descendants emit folds the inherited product into its alpha, so fading a panel fades
        // its text, widgets, and children as one. A fully faded subtree emits nothing.
        const f32 opacity = inherited * style.Opacity;
        if (opacity <= 0.0f)
        {
            return;
        }

        // A rotation rigidly turns the element's whole subtree about its Origin anchor at paint: push
        // the transform before the element's own primitives, keep it through the children, and pop
        // after. A zero rotation pushes nothing, so the identity path is exactly the unrotated
        // geometry. Layout, hit-testing, and clips are unaffected — this only rotates emitted
        // positions.
        const bool rotated = style.Rotation != 0.0f;
        if (rotated)
        {
            const vec2 pivot = rect.Min + style.Origin * rect.Size;
            list.PushTransform(pivot, glm::radians(style.Rotation));
        }

        // A drop shadow is one extra quad *behind* the fill — the element's own rounded box, its
        // silhouette displaced and grown, its edge softened across the blur. It is emitted inside
        // the element's draw, so it folds in the composited opacity and rides the transform and
        // clip stacks like every other primitive.
        if (style.Shadow.has_value() && !style.Shadow->Inset)
        {
            BoxShadow shadow = *style.Shadow;
            shadow.Color.a *= opacity;
            list.Shadow(rect, shadow, style.Radii);
        }

        // Fill sources are exclusive and ranked BackgroundMaterial > BackgroundGradient >
        // BackgroundImage > Background: the winning source is the fill, and they never layer. The
        // border is drawn over whichever wins.
        if (style.BackgroundMaterial.IsLoaded())
        {
            // A material emits the RGBA inside the shape; the engine's SDF coverage, the border
            // ring, and the composited opacity multiply into it, so the material never widens or
            // replaces the silhouette. The opacity rides the vertex color the fragment reads.
            list.MaterialFill(rect, style.BackgroundMaterial.Get(), style.Radii, {},
                              vec4(1.0f, 1.0f, 1.0f, opacity));
        }
        else if (style.BackgroundGradient.has_value() && style.BackgroundGradient->Ramp.IsLoaded())
        {
            const ResolvedGradient& gradient = *style.BackgroundGradient;
            const Texture& ramp = *gradient.Ramp.Get();
            list.Gradient(rect,
                          GradientFill{.Kind = gradient.Kind,
                                       .P0 = gradient.P0,
                                       .P1 = gradient.P1,
                                       .AngleOffset = gradient.AngleOffset,
                                       .Ramp = ramp.GetHandle(),
                                       .Sampler = ramp.GetSamplerHandle()},
                          style.Radii, {}, vec4(1.0f, 1.0f, 1.0f, opacity));
        }
        else if (style.BackgroundImage.IsLoaded())
        {
            const Texture& texture = *style.BackgroundImage.Get();
            const FillBox box = ToPaddingBox(rect, style);
            const vec2 source = vec2(texture.GetExtent());
            const vec4 tint = vec4(1.0f, 1.0f, 1.0f, opacity);
            if (IsSliced(style.BackgroundSlice))
            {
                // The slice insets author source-texture pixels; the primitive takes the source
                // split as UV fractions and keeps the destination corners at their source size.
                // `tile` repeats each stretchable cell within its own sub-rect (the corners stay
                // fixed), which the fragment wraps arithmetically rather than through the sampler.
                const Insets& slice = style.BackgroundSlice;
                list.NineSlice(box.Box, texture.GetHandle(), texture.GetSamplerHandle(),
                               SliceToUv(slice, source), slice, tint,
                               {.Min = {0.0f, 0.0f}, .Size = {1.0f, 1.0f}}, style.BackgroundRepeat,
                               source);
            }
            else if (style.BackgroundRepeat == ImageRepeat::Tile)
            {
                // One quad with the UV rect scaled by box / texture size, tiled by the texture's own
                // wrapping sampler — never a quad per tile, which would be unbounded against the
                // draw list's fixed geometry ring.
                const Rect uv{.Min = vec2(0.0f),
                              .Size = TileUvSize(box.Box.Size, source, vec2(1.0f))};
                list.Texture(box.Box, texture.GetHandle(), texture.GetSamplerHandle(), uv, tint,
                             box.Radii);
            }
            else
            {
                const FittedFill fill = FitTexture(box.Box, source, style.BackgroundFit);
                list.Texture(fill.Dest, texture.GetHandle(), texture.GetSamplerHandle(), fill.Uv,
                             tint, box.Radii);
            }
        }
        else if (style.Background.a > 0.0f)
        {
            vec4 background = style.Background;
            background.a *= opacity;
            list.Quad(rect, background, style.Radii);
        }
        // An Image paints its resident texture into its *content* box — inside the border and the
        // padding, the box its intrinsic measure sized and the box a Text leaf's run draws in —
        // over any background and under the border. It composes with corner-radius through the
        // shape SDF the same way a Panel background does; the border below draws over it as a
        // frame. The tint folds in the composited opacity, so a faded Image fades its texture too.
        //
        // The three shapes are the background fill's, against the widget's own properties: sliced
        // (nine-slice, unrounded), tiled (one quad, a wrapping sampler, a scaled UV), or fitted.
        //
        // An authored `material` supersedes the texture fill: the material shades the same content
        // box, with the element's own texture reaching it as a declared parameter rather than as
        // the fill itself, so a shader animates or recolors authored art instead of replacing it.
        if (element.Kind == ElementKind::Image && style.ImageMaterial.IsLoaded())
        {
            vec4 tint = element.ImageTint;
            tint.a *= opacity;
            const FillBox content = ToContentBox(rect, style);
            list.MaterialFill(content.Box, style.ImageMaterial.Get(), content.Radii, {}, tint,
                              element.ImageUv);
        }
        else if (element.Kind == ElementKind::Image && element.ImageTexture.IsValid() &&
                 element.ImageSampler.IsValid())
        {
            vec4 tint = element.ImageTint;
            tint.a *= opacity;
            const FillBox content = ToContentBox(rect, style);
            // Fit and slice are computed against the *sampled* sub-rect, so an atlas flipbook frame
            // fits and slices its own cell. An *unsliced* tile repeats the whole texture, which is
            // what the sampler's wrap addresses; a sliced one repeats each cell within its own
            // sub-rect, which only the fragment's arithmetic wrap can express.
            const vec2 sampled = element.ImageSize * element.ImageUv.Size;
            if (IsSliced(style.ImageSlice))
            {
                const Insets& slice = style.ImageSlice;
                list.NineSlice(content.Box, element.ImageTexture, element.ImageSampler,
                               SliceToUv(slice, sampled), slice, tint, element.ImageUv,
                               style.ImageRepeatMode, sampled);
            }
            else if (style.ImageRepeatMode == ImageRepeat::Tile)
            {
                const Rect uv{
                    .Min = element.ImageUv.Min,
                    .Size = TileUvSize(content.Box.Size, element.ImageSize, element.ImageUv.Size)};
                list.Texture(content.Box, element.ImageTexture, element.ImageSampler, uv, tint,
                             content.Radii);
            }
            else
            {
                const FittedFill fill =
                    FitTexture(content.Box, sampled, style.ObjectFit, element.ImageUv);
                list.Texture(fill.Dest, element.ImageTexture, element.ImageSampler, fill.Uv, tint,
                             content.Radii);
            }
        }
        // An inset shadow paints *over* the fill instead of behind it, bounded by the box it
        // recesses — so it lands after every fill source and under the border ring.
        if (style.Shadow.has_value() && style.Shadow->Inset)
        {
            BoxShadow shadow = *style.Shadow;
            shadow.Color.a *= opacity;
            list.Shadow(rect, shadow, style.Radii);
        }
        if (style.BorderStyle.Width > 0.0f)
        {
            Border border = style.BorderStyle;
            border.Color.a *= opacity;
            list.Quad(rect, border.Color, style.Radii, border);
        }

        // An element clips when its overflow says so on either axis (a scrollable element always
        // does — the overflow is what it scrolls through), and a TextInput clips to its box
        // unconditionally so a value wider than the field stops at the frame instead of spilling
        // across its neighbours.
        const bool clip = ClipsContent(style) || element.Kind == ElementKind::TextInput;
        if (clip)
        {
            list.PushClip(rect);
        }

        // A control paints its own parts (a Slider's track and thumb, a ProgressBar's fill, a
        // TextInput's value and caret) between its background and its children.
        BuildWidget(element, list, opacity);

        const Font* const font = ResolveFont(element);
        if (!element.Text.empty() && font != nullptr &&
            (element.Kind == ElementKind::Text || element.Kind == ElementKind::Button))
        {
            // A Text leaf draws at its content-box origin (inside the border and padding, the box
            // the measure sized); a Button centers its label in its box.
            const f32 border = BorderWidth(style);
            vec2 origin = rect.Min + vec2(border + style.Padding.Left, border + style.Padding.Top);
            if (element.Kind == ElementKind::Button)
            {
                const vec2 label = MeasureElementText(element, std::nullopt);
                origin = rect.Min + (rect.Size - label) * 0.5f;
            }
            else if (style.TextAlignment != TextAlign::Left)
            {
                // Center/right alignment distributes the content box's slack ahead of the run — a
                // paint-only shift, so a content-sized box (no slack) draws exactly as Left does.
                const f32 content =
                    rect.Size.x - 2.0f * border - style.Padding.Left - style.Padding.Right;
                const f32 slack = content - MeasureElementText(element, std::nullopt).x;
                if (slack > 0.0f)
                {
                    origin.x += style.TextAlignment == TextAlign::Right ? slack : slack * 0.5f;
                }
            }
            vec4 textColor = style.TextColor;
            textColor.a *= opacity;
            // Shaped against the same width the measure was taken at, so the run painted here is
            // the run the box was sized for. A wrapping element hands its content width down; a
            // non-wrapping one hands nothing, which is what its measure did too.
            list.Text(origin, *font, element.Text, style.TextSize, textColor,
                      style.Wrapping == TextWrap::Wrap
                          ? optional<f32>{rect.Size.x - 2.0f * border - style.Padding.Left -
                                          style.Padding.Right}
                          : optional<f32>{});
        }

        for (const Element* child : element.Children)
        {
            BuildElement(*child, list, opacity);
        }

        if (clip)
        {
            list.PopClip();
        }

        if (rotated)
        {
            list.PopTransform();
        }
    }

    void Document::BuildWidget(const Element& element, DrawList& list, const f32 opacity) const
    {
        const Style& style = element.ComputedStyle;
        const Rect& rect = element.Layout;
        const auto faded = [opacity](vec4 color)
        {
            color.a *= opacity;
            return color;
        };

        if (element.Kind == ElementKind::TextInput)
        {
            BuildTextInput(element, list, opacity);
            return;
        }

        if (element.Kind == ElementKind::ProgressBar)
        {
            // The fill is the [0,1] value across the bar's width, drawn in the text color over the
            // background track. A bar with no explicit value renders empty.
            const f32 fraction = std::clamp(element.Widget.Value, 0.0f, 1.0f);
            if (fraction > 0.0f)
            {
                const Rect fill{.Min = rect.Min, .Size = vec2(rect.Size.x * fraction, rect.Size.y)};
                list.Quad(fill, faded(style.TextColor), style.Radii);
            }
            return;
        }
    }

    namespace
    {
        /// @brief The painted width of a TextInput's caret bar, in pixels.
        constexpr f32 CaretWidth = 1.0f;
    }

    void Document::BuildTextInput(const Element& element, DrawList& list, const f32 opacity) const
    {
        const Style& style = element.ComputedStyle;
        const Rect& rect = element.Layout;

        // The value draws inside the content box (past the border and padding) and rides the middle
        // of it. The line box is one line of the field's typography — the same extent the measure
        // reserves — so an intrinsically sized field centers with no slack and a taller styled box
        // keeps the run off its frame.
        const Font* const font = ResolveFont(element);
        const f32 inset = BorderWidth(style);
        const vec2 contentMin =
            rect.Min + vec2(inset + style.Padding.Left, inset + style.Padding.Top);
        const f32 contentHeight =
            rect.Size.y - 2.0f * inset - style.Padding.Top - style.Padding.Bottom;
        const f32 line = MeasureRun("", font, style, std::nullopt, true).y;
        const vec2 origin(contentMin.x, contentMin.y + std::max(contentHeight - line, 0.0f) * 0.5f);

        if (!element.Text.empty() && font != nullptr)
        {
            vec4 textColor = style.TextColor;
            textColor.a *= opacity;
            list.Text(origin, *font, element.Text, style.TextSize, textColor);
        }

        // The caret marks the edit position while the field holds focus: a thin bar at the width of
        // the value's prefix up to the caret's codepoint index. It is a plain quad, so an empty
        // field still shows where the next codepoint lands.
        if ((element.State & ElementState::Focused) == ElementState::None)
        {
            return;
        }

        const vector<u32> codepoints = DecodeUtf8(element.Text);
        const u32 caret = std::min(element.Widget.Caret, static_cast<u32>(codepoints.size()));
        string prefix;
        for (u32 i = 0; i < caret; ++i)
        {
            AppendUtf8(prefix, codepoints[i]);
        }

        vec4 caretColor = style.TextColor;
        caretColor.a *= opacity;
        const f32 caretX = MeasureRun(prefix, font, style, std::nullopt, false).x;
        list.Quad(Rect{.Min = vec2(origin.x + caretX, origin.y), .Size = vec2(CaretWidth, line)},
                  caretColor);
    }

    void Document::Build(DrawList& list) const
    {
        BuildElement(*m_Root, list, 1.0f);

        // The main walk's clip and transform pushes are balanced, so both stacks are back at the
        // caller's state here: a popup inherits no ancestor scissor and no ancestor rotation, and
        // paints over every main-tree primitive. Bottom-up, so a submenu covers its menu. A popup
        // that clips or rotates still pushes its own, inside its own walk.
        for (const Popup& popup : m_Popups)
        {
            BuildElement(*popup.Root, list, 1.0f);
        }
    }

    void Document::Drive(vec2 available, f32 delta, DrawList& out)
    {
        Update(delta);
        Solve(available);
        Build(out);
        // Build has just read every paint-only property into the draw list, so anything written
        // before this point is now on screen. Solve clears the layout flag itself.
        m_PaintDirty = false;
    }

    namespace
    {
        // Returns whether a point lies inside a rect (inclusive of the min edge, exclusive of max).
        bool Contains(const Rect& rect, vec2 point)
        {
            return point.x >= rect.Min.x && point.y >= rect.Min.y && point.x < rect.Max().x &&
                   point.y < rect.Max().y;
        }
    }

    Element* Document::HitTestElement(Element& element, vec2 point, optional<Rect> clip)
    {
        if (!element.Visible)
        {
            return nullptr;
        }

        // A pointer-events:none element is transparent to hit-testing, subtree included, so a
        // display-only overlay piece never occludes what drives it.
        if (element.ComputedStyle.Pointer == PointerEvents::None)
        {
            return nullptr;
        }

        // A clipping element hides the parts of its subtree outside its box; a point outside the
        // active clip cannot hit this element or its descendants.
        if (clip && !Contains(*clip, point))
        {
            return nullptr;
        }

        optional<Rect> childClip = clip;
        if (ClipsContent(element.ComputedStyle))
        {
            childClip = clip ? clip->Intersect(element.Layout) : element.Layout;
        }

        // Children paint over the parent (and later children over earlier ones), so the last child
        // under the point is the topmost — walk children back-to-front for front-to-back hit order.
        for (auto it = element.Children.rbegin(); it != element.Children.rend(); ++it)
        {
            if (Element* hit = HitTestElement(**it, point, childClip))
            {
                return hit;
            }
        }

        // Children were tested above, so a pointer-events:children element only has to decline
        // itself: the backdrop passes the pointer through while its controls still claim it.
        if (element.ComputedStyle.Pointer != PointerEvents::Children &&
            Contains(element.Layout, point))
        {
            return &element;
        }
        return nullptr;
    }

    void Document::SetHovered(Element* const target)
    {
        for (Element* const previous : m_Hovered)
        {
            SetState(*previous, WithBit(previous->State, ElementState::Hovered, false));
        }
        m_Hovered.clear();
        m_HoverTarget = target;
        if (target == nullptr)
        {
            return;
        }

        // Hover is a property of a **box**, so every box the pointer is inside is hovered: the
        // element under it, every ancestor containing it, and the pointer-transparent content that
        // element draws.
        //
        // The last term is what a composite control needs. A row that is one button spelled as a
        // button wrapping the texts inside it cannot restyle those texts on hover otherwise: a
        // `pointer-events: none` subtree is pruned from hit-testing outright, so nothing in it can
        // ever be a hit target and nothing in it could carry the bit on its own — and the selector
        // grammar has no descendant combinator to reach it with either. So its host's state is the
        // only state it has, which is the same move `Selected` already makes across an item slot.
        // A descendant that *is* reachable is skipped: it is hovered when the pointer is over it,
        // and marking it because a sibling is would be a different claim entirely.
        const auto mark = [this](Element& element)
        {
            SetState(element, WithBit(element.State, ElementState::Hovered, true));
            m_Hovered.push_back(&element);
        };
        const auto markContent = [&mark](auto&& self, Element& element) -> void
        {
            mark(element);
            for (Element* const child : element.Children)
            {
                self(self, *child);
            }
        };
        mark(*target);
        for (Element* const child : target->Children)
        {
            if (child->ComputedStyle.Pointer == PointerEvents::None)
            {
                markContent(markContent, *child);
            }
        }
        for (Element* ancestor = target->Parent; ancestor != nullptr; ancestor = ancestor->Parent)
        {
            mark(*ancestor);
        }
    }

    Element* Document::HitTest(vec2 point)
    {
        // Popups paint over the main tree, so they claim the pointer over it: try the stack
        // top-down first, each against no inherited clip, and only then the main tree. This is
        // also what makes IsPointerOverDocument count an open menu covering the content below it.
        for (auto it = m_Popups.rbegin(); it != m_Popups.rend(); ++it)
        {
            if (Element* const hit = HitTestElement(*it->Root, point, std::nullopt))
            {
                return hit;
            }
        }
        return HitTestElement(*m_Root, point, std::nullopt);
    }

    ElementHandle Document::GetHandle(const Element& element) const
    {
        return ElementHandle{.Value = element.Serial};
    }

    Element* Document::Resolve(const ElementHandle handle)
    {
        if (!handle.IsValid())
        {
            return nullptr;
        }
        const auto it = std::ranges::find_if(m_Elements, [&](const Unique<Element>& owned)
                                             { return owned->Serial == handle.Value; });
        return it != m_Elements.end() ? it->get() : nullptr;
    }

    bool Document::IsInSubtree(const Element& element, const Element& root)
    {
        for (const Element* cursor = &element; cursor != nullptr; cursor = cursor->Parent)
        {
            if (cursor == &root)
            {
                return true;
            }
        }
        return false;
    }

    PopupId Document::OpenPopup(Element& anchor, const PopupOptions& options)
    {
        Element& root = CreateElement(ElementKind::Panel);

        // A popup root has no parent, so the typography that inherits down a subtree has no chain
        // to walk: seed it from the anchor's resolved font, which is the font the popup would have
        // inherited had it been parented where it is anchored.
        for (const Element* cursor = &anchor; cursor != nullptr; cursor = cursor->Parent)
        {
            if (cursor->ComputedStyle.TextFont.IsLoaded())
            {
                root.BaseStyle.TextFont = cursor->ComputedStyle.TextFont;
                root.ComputedStyle.TextFont = cursor->ComputedStyle.TextFont;
                break;
            }
        }

        m_Popups.emplace_back(Popup{
            .Id = m_NextPopupId++,
            .Root = &root,
            .Anchor = GetHandle(anchor),
            .AnchorElement = &anchor,
            .Options = options,
            .RestoreFocus = m_Focused != nullptr ? GetHandle(*m_Focused) : ElementHandle{},
        });
        m_Dirty = true;
        return PopupId{.Value = m_Popups.back().Id};
    }

    Element* Document::GetPopupRoot(const PopupId id)
    {
        const auto it = std::ranges::find_if(m_Popups, [&](const Popup& popup)
                                             { return popup.Id == id.Value; });
        return it != m_Popups.end() ? it->Root : nullptr;
    }

    bool Document::IsPopupOpen(const PopupId id) const
    {
        return id.IsValid() && std::ranges::any_of(m_Popups, [&](const Popup& popup)
                                                   { return popup.Id == id.Value; });
    }

    PopupId Document::GetTopPopup() const
    {
        return m_Popups.empty() ? PopupId{} : PopupId{.Value = m_Popups.back().Id};
    }

    void Document::ClosePopup(const PopupId id)
    {
        if (!id.IsValid())
        {
            return;
        }
        for (usize i = 0; i < m_Popups.size(); ++i)
        {
            if (m_Popups[i].Id == id.Value)
            {
                ClosePopupsFrom(i);
                return;
            }
        }
    }

    void Document::CloseAllPopups()
    {
        if (!m_Popups.empty())
        {
            ClosePopupsFrom(0);
        }
    }

    void Document::ClosePopupsFrom(const usize index)
    {
        // The stack is LIFO, so closing an entry dismisses everything above it. The pre-open focus
        // to restore is the *lowest* closed popup's — the state before the whole chain opened.
        const ElementHandle restore = m_Popups[index].RestoreFocus;
        while (m_Popups.size() > index)
        {
            // Pop before destroying: the destroy walk closes popups anchored inside the subtree it
            // frees, and this entry must already be off the stack when that runs.
            Element* const root = m_Popups.back().Root;
            m_Popups.pop_back();
            DestroySubtree(*root);
        }

        if (Element* const previous = Resolve(restore))
        {
            SetFocus(previous);
        }
        m_Dirty = true;
    }

    void Document::ForgetElement(const Element& element)
    {
        // A popup outliving its anchor would place itself against a freed rect, so the anchor's
        // destruction is the popup's dismissal — the mechanism behind "a popup closes with its
        // anchor" for the repeater case, where a shrinking bound array destroys whole item
        // subtrees under the document's feet.
        for (usize i = 0; i < m_Popups.size(); ++i)
        {
            if (m_Popups[i].AnchorElement == &element || m_Popups[i].Root == &element)
            {
                ClosePopupsFrom(i);
                break;
            }
        }

        if (m_Focused == &element)
        {
            m_Focused = nullptr;
        }
        if (m_HoverTarget == &element)
        {
            m_HoverTarget = nullptr;
        }
        std::erase(m_Hovered, &element);
        if (m_PressTarget == &element)
        {
            m_PressTarget = nullptr;
        }
        if (m_PressOrigin == &element)
        {
            m_PressOrigin = nullptr;
        }
    }

    void Document::BindContext(BindingContext* context, const TypeRegistry* registry)
    {
        VE_ASSERT(context == nullptr || registry != nullptr,
                  "BindContext requires a TypeRegistry when a context is bound");
        m_Context = context;
        m_Registry = registry;
        // Force a re-read on the next UpdateBindings by resetting the resolved-version watermark.
        m_BoundVersion = 0;
    }

    void Document::BindContext(BindingContext* context)
    {
        // Resolve the registry from the document's own AssetManager, so a caller holding no
        // TypeRegistry (a GuiOverlay driver) binds its view-model without re-supplying it.
        const TypeRegistry* const registry =
            context != nullptr && m_Assets != nullptr ? &m_Assets->GetTypeRegistry() : nullptr;
        BindContext(context, registry);
    }

    namespace
    {
        // A `value` binding on a value-bearing control (Slider/Checkbox/ProgressBar) writes the
        // resolved scalar into the widget state; on any other kind it writes the element's text.
        bool IsValueWidget(ElementKind kind)
        {
            return kind == ElementKind::Slider || kind == ElementKind::Checkbox ||
                   kind == ElementKind::ProgressBar;
        }
    }

    void Document::ResolveElementBindings(Element& element)
    {
        for (const auto& [property, expression] : element.Bindings)
        {
            // Only `{obj.field}` value bindings resolve here; a handler entry (onClick, …) is keyed
            // by an event name and fired by the event path, not written as a value. The min/max/step
            // widget config carries a literal (not a path) and is read at instantiate time, not here.
            if (property != "text" && property != "value" && property != "visible")
            {
                // A binding may instead target a bindable paint property ("background", "color",
                // "border-color", "opacity"): the resolved numeric field writes the element's
                // base style directly — paint-only, so no layout re-solve.
                if (const optional<StyleProperty> styleProperty = ParseStyleProperty(property);
                    styleProperty.has_value() && IsBindableStyleProperty(*styleProperty))
                {
                    if (const optional<vec4> value =
                            ResolveNumericLeaf(*m_Registry, m_Context->GetData(),
                                               m_Context->GetDataType(), expression);
                        value.has_value())
                    {
                        WriteProperty(element.BaseStyle, *styleProperty, *value);
                        WriteProperty(element.ComputedStyle, *styleProperty, *value);
                    }
                }
                continue;
            }

            const optional<string> resolved = ResolvePath(*m_Registry, m_Context->GetData(),
                                                          m_Context->GetDataType(), expression);
            if (!resolved)
            {
                continue;
            }

            if (property == "visible")
            {
                SetVisible(element, *resolved == "true" || *resolved == "1");
            }
            else if (property == "value" && IsValueWidget(element.Kind))
            {
                // A control's value binding is one-way: reflect the model into the widget without
                // firing onChange (the change came from the model, not the user). SetWidgetValue
                // fires onChange, so write the widget state directly here.
                f32 value = 0.0f;
                const string& text = *resolved;
                if (text == "true")
                {
                    value = 1.0f;
                }
                else if (text != "false")
                {
                    static_cast<void>(
                        std::from_chars(text.data(), text.data() + text.size(), value));
                }
                if (element.Kind == ElementKind::Checkbox)
                {
                    element.Widget.Value = value != 0.0f ? 1.0f : 0.0f;
                    SetState(element, WithBit(element.State, ElementState::Checked, value != 0.0f));
                }
                else
                {
                    element.Widget.Value = ClampStep(value, element.Widget.Min, element.Widget.Max,
                                                     element.Widget.Step);
                }
            }
            else if (property == "value" && element.Kind == ElementKind::TextInput)
            {
                if (*resolved != element.Text)
                {
                    // A model-driven value lands with the caret past its last codepoint, the same
                    // place InitWidget puts it, so typing into a prefilled field appends.
                    element.Widget.Caret = static_cast<u32>(DecodeUtf8(*resolved).size());
                    SetText(element, *resolved);
                }
            }
            else
            {
                SetText(element, *resolved);
            }
        }
    }

    void Document::UpdateBindings()
    {
        if (m_Context == nullptr || m_Context->GetData() == nullptr || m_Registry == nullptr)
        {
            return;
        }
        if (m_Context->GetVersion() == m_BoundVersion)
        {
            return;
        }
        m_BoundVersion = m_Context->GetVersion();

        // Re-materialize each List's item children against its bound array first — this may add or
        // remove elements, so it runs before the flat binding walk over m_Elements below and its
        // freshly-created items already carry their per-item resolved values.
        SyncLists();

        // Resolve every non-List-item element's bindings against the main context. A List item's own
        // bindings resolve against its array element inside SyncList, so they are skipped here.
        for (const Unique<Element>& element : m_Elements)
        {
            if (!element->Bindings.empty() && !IsListItem(*element))
            {
                ResolveElementBindings(*element);
            }
        }
    }

    namespace
    {
        // Whether an element repeats its authored children per bound array element. A List always
        // does; a Table only when it carries an `items` binding — a static Table's children are
        // hand-authored rows whose bindings resolve against the main context.
        bool IsItemHost(const Element& element)
        {
            return element.Kind == ElementKind::List ||
                   (element.Kind == ElementKind::Table &&
                    element.Bindings.find("items") != element.Bindings.end());
        }
    }

    bool Document::IsListItem(const Element& element) const
    {
        for (const Element* e = element.Parent; e != nullptr; e = e->Parent)
        {
            if (IsItemHost(*e))
            {
                return true;
            }
        }
        return false;
    }

    void Document::SyncLists()
    {
        // Collect the repeaters first: SyncList mutates m_Elements (adding/removing item clones), so
        // the walk cannot iterate m_Elements directly.
        vector<Element*> lists;
        for (const Unique<Element>& element : m_Elements)
        {
            if (IsItemHost(*element))
            {
                lists.push_back(element.get());
            }
        }
        for (Element* list : lists)
        {
            SyncList(*list);
        }
    }

    void Document::SyncList(Element& list)
    {
        const auto binding = list.Bindings.find("items");
        if (binding == list.Bindings.end())
        {
            return;
        }

        // On first sync, lift the authored children out of the live tree into the template store —
        // they are the item template, cloned per array element, never laid out or drawn themselves.
        if (m_ListTemplates.find(&list) == m_ListTemplates.end())
        {
            ListTemplate captured;
            const std::span<Element* const> content = ContentChildren(list);
            const vector<Element*> authored(content.begin(), content.end());
            const YGNodeRef listNode = m_Yoga->Get(list);
            for (Element* child : authored)
            {
                // Unlink the authored child from the list's layout node before detaching its subtree
                // (DetachTemplate frees the child nodes), so the list node never references a freed
                // node.
                if (listNode != nullptr)
                {
                    if (const YGNodeRef childNode = m_Yoga->Get(*child); childNode != nullptr)
                    {
                        YGNodeRemoveChild(listNode, childNode);
                    }
                }
                captured.Roots.push_back(DetachTemplate(*child, captured.Owned));
            }
            // Only the content is lifted into the template; a scrollbar the widget layer already
            // created sits in the tail and stays a live child of the list.
            list.Children.erase(list.Children.begin(),
                                list.Children.begin() +
                                    static_cast<std::ptrdiff_t>(authored.size()));
            m_ListTemplates.emplace(&list, std::move(captured));
        }

        const optional<ResolvedField> field = ResolveFieldPtr(
            *m_Registry, m_Context->GetData(), m_Context->GetDataType(), binding->second);
        if (!field || field->Field->Class != FieldClass::Array ||
            field->Field->ArraySize == nullptr)
        {
            return;
        }

        const usize count = field->Field->ArraySize(field->Ptr);
        const vector<Element*>& roots = m_ListTemplates.at(&list).Roots;
        const usize perItem = roots.size();
        const usize haveItems = perItem == 0 ? 0 : ContentChildren(list).size() / perItem;

        // Grow: append a fresh clone of each template root per new array element.
        for (usize i = haveItems; i < count; ++i)
        {
            for (const Element* node : roots)
            {
                static_cast<void>(CloneTemplate(list, *node));
            }
        }
        // Shrink: remove the trailing item clones for the elements that went away.
        for (usize i = count; i < haveItems; ++i)
        {
            for (usize k = 0; k < perItem; ++k)
            {
                // The content tail, not Children.back() — a scrollbar sits after the items.
                const std::span<Element* const> content = ContentChildren(list);
                Remove(*content.back());
            }
        }

        // The selection indexes the bound array, so it survives a re-sync — but a shrink drops the
        // items it named and every item element was rebuilt, so it is re-clamped and its state bits
        // re-projected here. A re-sync is model-driven rather than a user act, so it notifies no
        // handler: the game already knows its own array changed.
        ApplyItemFocusability(list);
        if (list.Widget.Selection != SelectionMode::None)
        {
            WriteSelection(list, list.Widget.SelectedItems, false);
        }

        // Resolve each item's per-item bindings against its array element.
        for (usize i = 0; i < count && perItem != 0; ++i)
        {
            void* itemPtr = field->Field->ArrayElement(field->Ptr, i);
            for (usize k = 0; k < perItem; ++k)
            {
                Element& itemRoot = *ContentChildren(list)[i * perItem + k];
                ResolveItemBindings(itemRoot, itemPtr, field->Field->ElementType);
            }
        }
    }

    Element* Document::DetachTemplate(Element& element, vector<Unique<Element>>& owned)
    {
        ForgetElement(element);

        // Copy the element into a standalone template node held by `owned`, then recurse so the whole
        // subtree is captured; a template is inert data, never solved or drawn. The live element's
        // Yoga node and live storage are released here; the caller has already unlinked the root from
        // its parent node.
        auto node = CreateUnique<Element>();
        node->Kind = element.Kind;
        node->Id = element.Id;
        node->Classes = element.Classes;
        node->Text = element.Text;
        node->Bindings = element.Bindings;
        node->BaseStyle = element.BaseStyle;
        node->ComputedStyle = element.ComputedStyle;
        node->Variants = element.Variants;
        node->Transitions = element.Transitions;
        node->Focusable = element.Focusable;
        node->Visible = element.Visible;

        Element* result = node.get();
        owned.push_back(std::move(node));

        const YGNodeRef selfNode = m_Yoga->Get(element);
        const vector<Element*> children = element.Children;
        for (Element* child : children)
        {
            // Unlink the child from this node before its subtree's nodes are freed, so this node
            // never lists a freed child.
            if (selfNode != nullptr)
            {
                if (const YGNodeRef childNode = m_Yoga->Get(*child); childNode != nullptr)
                {
                    YGNodeRemoveChild(selfNode, childNode);
                }
            }
            Element* childNode = DetachTemplate(*child, owned);
            childNode->Parent = result;
            result->Children.push_back(childNode);
        }

        // Free the live element's Yoga node (already unlinked from its parent) and drop it from live
        // storage. Children were unlinked and freed above, so this node has no live children.
        m_Yoga->Destroy(element);
        const auto it = std::ranges::find_if(m_Elements, [&](const Unique<Element>& e)
                                             { return e.get() == &element; });
        if (it != m_Elements.end())
        {
            m_Elements.erase(it);
        }
        return result;
    }

    Element& Document::CloneTemplate(Element& parent, const Element& node)
    {
        Element& live = Add(parent, node.Kind);
        live.Id = node.Id;
        live.Classes = node.Classes;
        live.Text = node.Text;
        live.Bindings = node.Bindings;
        live.BaseStyle = node.BaseStyle;
        live.ComputedStyle = node.ComputedStyle;
        live.Variants = node.Variants;
        live.Transitions = node.Transitions;
        live.Focusable = node.Focusable;
        live.Visible = node.Visible;
        InitWidget(live);
        for (const Element* child : node.Children)
        {
            static_cast<void>(CloneTemplate(live, *child));
        }
        return live;
    }

    void Document::ResolveItemBindings(Element& element, void* itemBase, TypeId itemType)
    {
        for (const auto& [property, expression] : element.Bindings)
        {
            if (property != "text" && property != "value" && property != "visible")
            {
                // Per-item paint bindings (a row tint, a swatch) resolve against the array
                // element, mirroring the main-context style-binding path.
                if (const optional<StyleProperty> styleProperty = ParseStyleProperty(property);
                    styleProperty.has_value() && IsBindableStyleProperty(*styleProperty))
                {
                    if (const optional<vec4> value =
                            ResolveNumericLeaf(*m_Registry, itemBase, itemType, expression);
                        value.has_value())
                    {
                        WriteProperty(element.BaseStyle, *styleProperty, *value);
                        WriteProperty(element.ComputedStyle, *styleProperty, *value);
                    }
                }
                continue;
            }
            const optional<string> resolved =
                ResolvePath(*m_Registry, itemBase, itemType, expression);
            if (!resolved)
            {
                continue;
            }
            if (property == "visible")
            {
                SetVisible(element, *resolved == "true" || *resolved == "1");
            }
            else if (property == "value" && IsValueWidget(element.Kind))
            {
                f32 value = 0.0f;
                static_cast<void>(
                    std::from_chars(resolved->data(), resolved->data() + resolved->size(), value));
                element.Widget.Value = element.Kind == ElementKind::ProgressBar
                                           ? std::clamp(value, 0.0f, 1.0f)
                                           : value;
            }
            else
            {
                SetText(element, *resolved);
            }
        }
        for (Element* child : element.Children)
        {
            ResolveItemBindings(*child, itemBase, itemType);
        }
    }

    Element* Document::GetItemHost(const Element& element) const
    {
        for (Element* e = element.Parent; e != nullptr; e = e->Parent)
        {
            if (IsSelectionHost(e->Kind))
            {
                return e;
            }
        }
        return nullptr;
    }

    u32 Document::ItemStride(const Element& host) const
    {
        // A host repeating a bound array instantiates one clone of each template root per array
        // element, so a slot is that many children wide; a host with no template holds its
        // authored children one per slot.
        const auto it = m_ListTemplates.find(&host);
        if (it == m_ListTemplates.end() || it->second.Roots.empty())
        {
            return 1;
        }
        return static_cast<u32>(it->second.Roots.size());
    }

    u32 Document::GetItemCount(const Element& host) const
    {
        if (!IsSelectionHost(host.Kind))
        {
            return 0;
        }
        return static_cast<u32>(ContentChildren(host).size()) / ItemStride(host);
    }

    Element* Document::GetItemElement(const Element& host, const u32 index) const
    {
        if (!IsSelectionHost(host.Kind))
        {
            return nullptr;
        }
        const std::span<Element* const> content = ContentChildren(host);
        const usize slot = static_cast<usize>(index) * ItemStride(host);
        return slot < content.size() ? content[slot] : nullptr;
    }

    optional<u32> Document::GetItemIndex(const Element& element) const
    {
        const Element* const host = GetItemHost(element);
        if (host == nullptr)
        {
            return std::nullopt;
        }

        // Walk back down to the host's own child on the path — the item root the element sits
        // under, whatever depth inside the item template it was found at.
        const Element* item = &element;
        while (item->Parent != host)
        {
            item = item->Parent;
        }
        const std::span<Element* const> content = ContentChildren(*host);
        const auto it = std::ranges::find(content, item);
        if (it == content.end())
        {
            return std::nullopt;
        }
        return static_cast<u32>(it - content.begin()) / ItemStride(*host);
    }

    bool Document::IsItemSelected(const Element& host, const u32 index) const
    {
        return std::ranges::binary_search(host.Widget.SelectedItems, index);
    }

    void Document::RefreshItemSelectionStates(Element& host)
    {
        const u32 stride = ItemStride(host);
        const std::span<Element* const> content = ContentChildren(host);
        for (usize slot = 0; slot < content.size(); ++slot)
        {
            // Every element of a slot carries the bit, so a multi-root item paints as one
            // selected unit rather than only its first root.
            Element& item = *content[slot];
            const bool selected = IsItemSelected(host, static_cast<u32>(slot / stride));
            SetState(item, WithBit(item.State, ElementState::Selected, selected));
        }
    }

    void Document::ApplyItemFocusability(Element& host)
    {
        const bool selectable = host.Widget.Selection != SelectionMode::None;
        const u32 stride = ItemStride(host);
        const std::span<Element* const> content = ContentChildren(host);
        for (usize slot = 0; slot < content.size(); ++slot)
        {
            // One focus stop per item, on the slot's first element. An item root that is itself a
            // focusable control keeps its own focusability when the host is not selectable, so
            // turning selection off never demotes a Button item to unfocusable.
            Element& item = *content[slot];
            item.Focusable = (selectable && slot % stride == 0) || IsFocusableWidget(item.Kind);
        }
    }

    void Document::WriteSelection(Element& host, vector<u32> indices, const bool notify)
    {
        const u32 count = GetItemCount(host);
        std::erase_if(indices, [count](const u32 index) { return index >= count; });
        std::ranges::sort(indices);
        const auto duplicates = std::ranges::unique(indices);
        indices.erase(duplicates.begin(), duplicates.end());
        if (host.Widget.Selection == SelectionMode::Single && indices.size() > 1)
        {
            indices.resize(1);
        }

        const bool changed = indices != host.Widget.SelectedItems;
        host.Widget.SelectedItems = std::move(indices);
        RefreshItemSelectionStates(host);
        if (changed && notify)
        {
            static_cast<void>(FireHandler(host, "onSelectionChanged"));
        }
    }

    bool Document::ActivateItem(Element& host, const u32 index, const InputModifiers modifiers)
    {
        const SelectionMode mode = host.Widget.Selection;
        if (mode == SelectionMode::None || index >= GetItemCount(host))
        {
            return false;
        }

        const bool toggle =
            mode == SelectionMode::Multiple ||
            (mode == SelectionMode::Extended && (HasModifier(modifiers, InputModifiers::Control) ||
                                                 HasModifier(modifiers, InputModifiers::Meta)));
        const bool extend = mode == SelectionMode::Extended &&
                            HasModifier(modifiers, InputModifiers::Shift) &&
                            host.Widget.HasSelectionAnchor;

        vector<u32> next;
        if (extend)
        {
            // A range grows from the standing anchor, which therefore does not move — so a run of
            // Shift-clicks re-extends from the same origin rather than walking it forward.
            const u32 from = std::min(host.Widget.SelectionAnchor, index);
            const u32 to = std::max(host.Widget.SelectionAnchor, index);
            for (u32 i = from; i <= to; ++i)
            {
                next.push_back(i);
            }
        }
        else
        {
            if (toggle)
            {
                next = host.Widget.SelectedItems;
                if (const auto it = std::ranges::find(next, index); it != next.end())
                {
                    next.erase(it);
                }
                else
                {
                    next.push_back(index);
                }
            }
            else
            {
                next.push_back(index);
            }
            host.Widget.SelectionAnchor = index;
            host.Widget.HasSelectionAnchor = true;
        }

        WriteSelection(host, std::move(next), true);
        return true;
    }

    void Document::FocusItem(Element& item, const InputModifiers modifiers)
    {
        Element* const host = GetItemHost(item);
        if (host == nullptr || host->Widget.Selection == SelectionMode::None)
        {
            return;
        }
        const optional<u32> index = GetItemIndex(item);
        if (!index)
        {
            return;
        }

        // Single-select follows focus, and so does an unmodified Extended move — arrowing through
        // the list carries the selection with it. Control (or Meta) held moves focus alone, which
        // is what lets a user travel to an item and then toggle it; Multiple always moves focus
        // alone, since with no chord available its activation is the toggle.
        const SelectionMode mode = host->Widget.Selection;
        const bool detached = HasModifier(modifiers, InputModifiers::Control) ||
                              HasModifier(modifiers, InputModifiers::Meta);
        if (mode == SelectionMode::Single || (mode == SelectionMode::Extended && !detached))
        {
            static_cast<void>(ActivateItem(*host, *index, modifiers & InputModifiers::Shift));
        }
    }

    void Document::SetSelectionMode(Element& host, const SelectionMode mode)
    {
        if (!IsSelectionHost(host.Kind) || host.Widget.Selection == mode)
        {
            return;
        }
        host.Widget.Selection = mode;
        ApplyItemFocusability(host);

        // Narrowing the mode narrows what the standing selection may hold: None clears it and
        // Single keeps its first item. WriteSelection applies the truncation and re-projects the
        // state bits either way.
        vector<u32> retained =
            mode == SelectionMode::None ? vector<u32>{} : host.Widget.SelectedItems;
        WriteSelection(host, std::move(retained), true);
    }

    void Document::SetSelectedItems(Element& host, const std::span<const u32> indices)
    {
        if (!IsSelectionHost(host.Kind))
        {
            return;
        }
        WriteSelection(host, vector<u32>(indices.begin(), indices.end()), false);
    }

    void Document::SelectItem(Element& host, const u32 index, const bool selected)
    {
        if (!IsSelectionHost(host.Kind))
        {
            return;
        }

        vector<u32> next;
        if (!selected)
        {
            next = host.Widget.SelectedItems;
            std::erase(next, index);
        }
        else if (host.Widget.Selection == SelectionMode::Single)
        {
            next.push_back(index);
        }
        else
        {
            next = host.Widget.SelectedItems;
            next.push_back(index);
        }
        WriteSelection(host, std::move(next), false);
    }

    void Document::ClearSelection(Element& host)
    {
        if (IsSelectionHost(host.Kind))
        {
            WriteSelection(host, {}, false);
        }
    }

    void Document::ScrollIntoView(const Element& element)
    {
        Element* view = nullptr;
        for (Element* e = element.Parent; e != nullptr; e = e->Parent)
        {
            if (IsScrollable(e->ComputedStyle))
            {
                view = e;
                break;
            }
        }
        if (view == nullptr)
        {
            return;
        }

        // Layout rects already carry the view's scroll offset, so the shortfall past each edge is
        // exactly the delta to add. Pulling the near edge in wins when the element is taller than
        // the view and overflows both, which is the edge a reader reads from.
        const vec2 under = element.Layout.Min - view->Layout.Min;
        const vec2 over = element.Layout.Max() - view->Layout.Max();
        const auto axis = [](const f32 nearEdge, const f32 farEdge)
        { return nearEdge < 0.0f ? nearEdge : std::max(farEdge, 0.0f); };
        ScrollBy(*view, vec2(axis(under.x, over.x), axis(under.y, over.y)));
    }

    bool Document::FireHandler(Element& element, string_view event)
    {
        if (m_Context == nullptr)
        {
            return false;
        }
        const auto it = element.Bindings.find(string{event});
        if (it == element.Bindings.end())
        {
            return false;
        }
        const EventHandler* handler = m_Context->FindHandler(it->second);
        if (handler == nullptr || !*handler)
        {
            return false;
        }
        (*handler)(element);
        return true;
    }

    void Document::SetFocus(Element* element)
    {
        if (element != nullptr && !element->Focusable)
        {
            return;
        }
        if (element == m_Focused)
        {
            return;
        }
        if (m_Focused != nullptr)
        {
            SetState(*m_Focused, WithBit(m_Focused->State, ElementState::Focused, false));
        }
        m_Focused = element;
        if (m_Focused != nullptr)
        {
            SetState(*m_Focused, WithBit(m_Focused->State, ElementState::Focused, true));
        }
    }

    void Document::SetInteractive(bool interactive)
    {
        m_Interactive = interactive;
        if (!interactive)
        {
            // Popups belong to interactive documents — a display-only HUD opens none — so closing
            // interactivity dismisses whatever was open rather than stranding it on screen.
            CloseAllPopups();

            // Display-only documents hold no hover/press/focus state; drop any live interaction.
            SetHovered(nullptr);
            if (m_PressTarget != nullptr)
            {
                SetState(*m_PressTarget,
                         WithBit(m_PressTarget->State, ElementState::Active, false));
                m_PressTarget = nullptr;
            }
            if (m_PressOrigin != nullptr)
            {
                SetState(*m_PressOrigin,
                         WithBit(m_PressOrigin->State, ElementState::Active, false));
                m_PressOrigin = nullptr;
            }
            m_PressPanned = false;
            m_PointerDown = false;
            SetFocus(nullptr);
        }
    }

    bool Document::DispatchPointer(PointerEvent& event)
    {
        if (!m_Interactive)
        {
            return false;
        }

        // The pointer state a consumer drawing its own pointer reads, recorded before the routing
        // decides anything: an event that no element takes still moved the pointer and still
        // pressed the button, so both are recorded off the transition rather than off a target.
        m_PointerPosition = event.Position;
        if (event.Button == PointerButton::Primary)
        {
            if (event.Kind == PointerEventKind::Down)
            {
                m_PointerDown = true;
            }
            else if (event.Kind == PointerEventKind::Up)
            {
                m_PointerDown = false;
            }
        }

        Element* target = HitTest(event.Position);

        // Hover transitions: a move onto a new element leaves the old and enters the new, driving
        // the Hovered state a styling layer reads.
        if (target != m_HoverTarget)
        {
            SetHovered(target);
        }

        event.Target = target;

        // Light dismiss: a press that lands outside the top popup closes it (and everything above
        // it) and is consumed, so a click-away never doubles as a click on the content the popup
        // was covering. A press *inside* the popup falls through to the ordinary routing below.
        if (event.Kind == PointerEventKind::Down && !m_Popups.empty())
        {
            const Popup& top = m_Popups.back();
            if (top.Options.LightDismiss && (target == nullptr || !IsInSubtree(*target, *top.Root)))
            {
                ClosePopup(PopupId{.Value = top.Id});
                return true;
            }
        }

        if (event.Kind == PointerEventKind::Down && target != nullptr)
        {
            // A scrollable element claims a press that lands on one of its children — the
            // first-handler-wins capture that lets a drag started over a list item pan the
            // container rather than the item.
            Element* pressTarget = target;
            for (Element* e = target; e != nullptr; e = e->Parent)
            {
                // A scrollbar part sits inside the very element it scrolls, so it has to claim the
                // press before the walk reaches that element and turns the drag into a content pan.
                // A slider part deliberately does not claim: the walk continues to its Slider,
                // which maps the pointer to a value itself.
                if (IsScrollBarPart(e->Kind))
                {
                    pressTarget = e;
                    break;
                }
                if (IsScrollable(e->ComputedStyle))
                {
                    pressTarget = e;
                    break;
                }
                if (e->Kind == ElementKind::Slider)
                {
                    pressTarget = e;
                    break;
                }
            }

            m_PressTarget = pressTarget;
            m_PressOrigin = target;
            m_PressPanned = false;
            m_LastScrollPointer = event.Position;
            SetState(*pressTarget, WithBit(pressTarget->State, ElementState::Active, true));
            // The claimant took the capture, but the thing under the pointer is what was pressed —
            // so it wears the pressed state too. Without this a row inside a scrolling list is the
            // one control on screen that never lights under the finger, because its list holds the
            // capture that the styling reads.
            if (target != pressTarget)
            {
                SetState(*target, WithBit(target->State, ElementState::Active, true));
            }
            if (pressTarget->Focusable)
            {
                SetFocus(pressTarget);
            }
            // A press on a Slider sets its value from where the pointer landed.
            static_cast<void>(DriveWidgetPointer(*pressTarget, event));

            // A press anywhere inside a selectable host's item focuses the item root, so the
            // keyboard picks up where the pointer left off. The hit target is usually a leaf deep
            // inside the item template, and pointer capture stays with whatever claimed it above —
            // a ScrollView still pans — because focus and capture are separate.
            if (const Element* const host = GetItemHost(*target);
                host != nullptr && host->Widget.Selection != SelectionMode::None)
            {
                if (const optional<u32> index = GetItemIndex(*target))
                {
                    if (Element* const item = GetItemElement(*host, *index))
                    {
                        SetFocus(item);
                    }
                }
            }
        }

        // A drag over a captured Slider/ScrollView updates its value/offset before the routing below.
        if (event.Kind == PointerEventKind::Move && m_PressTarget != nullptr)
        {
            static_cast<void>(DriveWidgetPointer(*m_PressTarget, event));
        }

        // A Move only retargets hover (above); the discrete press/release/click transitions route
        // through the capture/bubble path where handlers fire and consume.
        bool consumed = false;
        if (event.Kind != PointerEventKind::Move)
        {
            consumed = RoutePointerPath(event);
        }

        if (event.Kind == PointerEventKind::Up)
        {
            if (m_PressTarget != nullptr)
            {
                SetState(*m_PressTarget,
                         WithBit(m_PressTarget->State, ElementState::Active, false));
            }
            if (m_PressOrigin != nullptr && m_PressOrigin != m_PressTarget)
            {
                SetState(*m_PressOrigin,
                         WithBit(m_PressOrigin->State, ElementState::Active, false));
            }
            // A press and its release on the same element is a click — on the element the press
            // *landed* on, which is not always the one that captured it. A scrollable element
            // claims its descendants' presses so a drag pans, and reading the click off the
            // claimant would mean no row inside a scrolling list could ever be clicked: the
            // release hit-tests the row, the capture holds the list, and the two never match.
            //
            // What the capture legitimately consumes is a press that *became* a scroll. So a pan
            // that actually moved the content cancels the click, which is what separates flicking
            // a list from picking an item out of it, and is why the pan is tracked rather than the
            // pointer's travel: the content moving is the gesture happening.
            if (m_PressOrigin != nullptr && m_PressOrigin == target && !m_PressPanned)
            {
                PointerEvent click{.Kind = PointerEventKind::Click,
                                   .Button = event.Button,
                                   .Position = event.Position,
                                   .Modifiers = event.Modifiers,
                                   .Target = target};
                RoutePointerPath(click);

                // A click inside a selectable host's item applies the selection chord before the
                // item's own activation below, so a Button inside an item both selects its row and
                // fires its onClick.
                if (Element* const host = GetItemHost(*target);
                    host != nullptr && event.Button == PointerButton::Primary)
                {
                    if (const optional<u32> index = GetItemIndex(*target))
                    {
                        static_cast<void>(ActivateItem(*host, *index, event.Modifiers));
                    }
                }
                // A Checkbox click toggles its bound value (which fires onChange); every other kind
                // fires onClick. A completed click is consumed whether or not a handler was
                // registered — the press was already claimed by this document on the Down.
                if (target->Kind == ElementKind::Checkbox)
                {
                    SetWidgetValue(*target, target->Widget.Value != 0.0f ? 0.0f : 1.0f);
                }
                else
                {
                    static_cast<void>(FireHandler(*target, "onClick"));
                }
                consumed = true;
            }
            m_PressTarget = nullptr;
            m_PressOrigin = nullptr;
            m_PressPanned = false;
        }

        return consumed || target != nullptr;
    }

    bool Document::DispatchScroll(const vec2 point, const vec2 delta)
    {
        if (!m_Interactive || delta == vec2(0.0f))
        {
            return false;
        }
        Element* const target = HitTest(point);
        if (target == nullptr)
        {
            return false;
        }

        // The wheel belongs to the nearest scrollable box under the pointer that can still move the
        // way it was turned. A box already at that end declines, so the turn passes outward to
        // whatever contains it — a list scrolled to its bottom goes on scrolling the panel it sits
        // in — and a document with nothing left to move consumes nothing at all, leaving the wheel
        // to whatever else was going to read it.
        for (Element* box = target; box != nullptr; box = box->Parent)
        {
            if (!IsScrollable(box->ComputedStyle))
            {
                continue;
            }
            const vec2 range = ScrollRange(*box);
            const vec2 offset = box->Widget.ScrollOffset;
            const auto moves = [](const f32 amount, const f32 at, const f32 extent)
            { return amount < 0.0f ? at > 0.0f : amount > 0.0f && at < extent; };
            const bool x = moves(delta.x, offset.x, range.x);
            const bool y = moves(delta.y, offset.y, range.y);
            if (!x && !y)
            {
                continue;
            }
            ScrollBy(*box, vec2(x ? delta.x : 0.0f, y ? delta.y : 0.0f));
            return true;
        }
        return false;
    }

    bool Document::RoutePointerPath(PointerEvent& event)
    {
        if (event.Target == nullptr)
        {
            return false;
        }

        // The ancestor path root→target; capture walks it forward, bubble reverse.
        vector<Element*> path;
        for (Element* e = event.Target; e != nullptr; e = e->Parent)
        {
            path.push_back(e);
        }
        std::ranges::reverse(path);

        for (Element* e : path)
        {
            event.Target = e;
            if (FireHandler(*e, "onPointerCapture"))
            {
                event.Handled = true;
            }
            if (event.Handled)
            {
                return true;
            }
        }
        for (auto it = path.rbegin(); it != path.rend(); ++it)
        {
            event.Target = *it;
            if (FireHandler(**it, "onPointer"))
            {
                event.Handled = true;
            }
            if (event.Handled)
            {
                return true;
            }
        }
        return false;
    }

    bool Document::Navigate(NavAction action, InputModifiers modifiers)
    {
        if (!m_Interactive)
        {
            return false;
        }

        // Cancel dismisses the top popup before anything else sees it: Esc (or gamepad B) closes
        // an open menu rather than firing the focused element's `onCancel` beneath it.
        if (action == NavAction::Cancel && !m_Popups.empty())
        {
            ClosePopup(PopupId{.Value = m_Popups.back().Id});
            return true;
        }

        if (action == NavAction::Confirm)
        {
            if (m_Focused == nullptr)
            {
                return false;
            }
            // A Confirm on a selectable host's item applies the selection chord, then falls
            // through to the item's own activation — so an item template rooted at a Button both
            // selects its row and fires its onClick, and a gamepad toggles a Multiple list with
            // no chord to press.
            bool selected = false;
            if (Element* const host = GetItemHost(*m_Focused); host != nullptr)
            {
                if (const optional<u32> index = GetItemIndex(*m_Focused))
                {
                    selected = ActivateItem(*host, *index, modifiers);
                }
            }
            // A Checkbox Confirm toggles its bound value the same way a click does — one path.
            if (m_Focused->Kind == ElementKind::Checkbox)
            {
                SetWidgetValue(*m_Focused, m_Focused->Widget.Value != 0.0f ? 0.0f : 1.0f);
                return true;
            }
            // Confirm activates the focused element the same way a click does — one handler path.
            SetState(*m_Focused, WithBit(m_Focused->State, ElementState::Active, true));
            const bool fired = FireHandler(*m_Focused, "onClick");
            SetState(*m_Focused, WithBit(m_Focused->State, ElementState::Active, false));
            return fired || selected;
        }
        if (action == NavAction::Cancel)
        {
            return m_Focused != nullptr && FireHandler(*m_Focused, "onCancel");
        }

        // A directional action on a focused Slider nudges its value, and on a scrollable element
        // scrolls it, rather than moving focus off it.
        if (m_Focused != nullptr &&
            (m_Focused->Kind == ElementKind::Slider || IsScrollable(m_Focused->ComputedStyle)))
        {
            if (DriveWidgetNavigation(*m_Focused, action))
            {
                return true;
            }
        }

        // An open popup scopes focus navigation to itself: the stops behind a menu are not
        // reachable while it covers them, exactly as they are not clickable.
        vector<Element*> focusables;
        GatherFocusables(m_Popups.empty() ? *m_Root : *m_Popups.back().Root, focusables);
        if (focusables.empty())
        {
            return false;
        }

        // Every focus move runs the same tail: an item that takes focus applies its host's
        // follow-focus selection rule, and a focused element inside a ScrollView is revealed.
        const auto moveFocusTo = [&](Element* element)
        {
            SetFocus(element);
            if (m_Focused != nullptr)
            {
                FocusItem(*m_Focused, modifiers);
                ScrollIntoView(*m_Focused);
            }
        };

        // With nothing focused, any navigation lands on the first focusable.
        if (m_Focused == nullptr)
        {
            moveFocusTo(focusables.front());
            return true;
        }

        if (action == NavAction::Next || action == NavAction::Previous)
        {
            const auto it = std::ranges::find(focusables, m_Focused);
            const usize index = it == focusables.end() ? 0 : usize(it - focusables.begin());
            const usize count = focusables.size();
            const usize next =
                action == NavAction::Next ? (index + 1) % count : (index + count - 1) % count;
            moveFocusTo(focusables[next]);
            return true;
        }

        // Directional: pick the nearest focusable whose center lies in the pressed direction.
        const vec2 from = m_Focused->Layout.Center();
        Element* best = nullptr;
        f32 bestScore = 0.0f;
        for (Element* candidate : focusables)
        {
            if (candidate == m_Focused)
            {
                continue;
            }
            const vec2 to = candidate->Layout.Center();
            const vec2 delta = to - from;

            bool inDirection = false;
            f32 primary = 0.0f;
            f32 secondary = 0.0f;
            switch (action)
            {
            case NavAction::MoveUp:
                inDirection = delta.y < 0.0f;
                primary = -delta.y;
                secondary = std::abs(delta.x);
                break;
            case NavAction::MoveDown:
                inDirection = delta.y > 0.0f;
                primary = delta.y;
                secondary = std::abs(delta.x);
                break;
            case NavAction::MoveLeft:
                inDirection = delta.x < 0.0f;
                primary = -delta.x;
                secondary = std::abs(delta.y);
                break;
            case NavAction::MoveRight:
                inDirection = delta.x > 0.0f;
                primary = delta.x;
                secondary = std::abs(delta.y);
                break;
            default:
                break;
            }
            if (!inDirection)
            {
                continue;
            }
            // Prefer the smallest travel along the axis, penalizing off-axis spread so a candidate
            // roughly in line wins over a nearer but far-to-the-side one.
            const f32 score = primary + secondary * 2.0f;
            if (best == nullptr || score < bestScore)
            {
                best = candidate;
                bestScore = score;
            }
        }
        if (best == nullptr)
        {
            return false;
        }
        moveFocusTo(best);
        return true;
    }

    bool Document::DispatchText(u32 codepoint)
    {
        if (!m_Interactive || m_Focused == nullptr)
        {
            return false;
        }

        // A focused TextInput edits its own text (insert/backspace/delete) and needs no game handler;
        // it fires onChange so a two-way binding writes back.
        if (m_Focused->Kind == ElementKind::TextInput)
        {
            return DriveWidgetText(*m_Focused, codepoint);
        }

        if (m_Context == nullptr)
        {
            return false;
        }
        const auto it = m_Focused->Bindings.find("onText");
        if (it == m_Focused->Bindings.end())
        {
            return false;
        }
        const EventHandler* handler = m_Context->FindHandler(it->second);
        if (handler == nullptr || !*handler)
        {
            return false;
        }
        // The widget layer's text handler reads the codepoint off the document; a bare EventHandler
        // carries only the element, so the codepoint rides a transient field the handler reads.
        m_PendingCodepoint = codepoint;
        (*handler)(*m_Focused);
        m_PendingCodepoint = 0;
        return true;
    }

    bool Document::DispatchTextEdit(TextEditAction action)
    {
        if (!m_Interactive || m_Focused == nullptr || m_Focused->Kind != ElementKind::TextInput)
        {
            return false;
        }
        return DriveWidgetTextEdit(*m_Focused, action);
    }

    void Document::GatherFocusables(Element& element, vector<Element*>& out) const
    {
        if (!element.Visible)
        {
            return;
        }
        if (element.Focusable)
        {
            out.push_back(&element);
        }
        for (Element* child : ContentChildren(element))
        {
            GatherFocusables(*child, out);
        }
    }
}
