#include <Veng/Gui/Document.h>

#include "YogaTree.h"

#include <Veng/Assert.h>
#include <Veng/Asset/Font.h>
#include <Veng/Gui/StyleSheet.h>
#include <Veng/Gui/UIDocument.h>

#include <algorithm>

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
    }

    Document::Document()
    {
        m_Yoga = CreateUnique<YogaTree>();
        YGConfigSetContext(m_Yoga->Config, this);
        m_Root = &CreateElement(ElementKind::Panel);
    }

    Document::~Document() = default;

    namespace
    {
        // Copies a recipe element's authored identity, text, inline style, bindings, and handlers
        // onto a live element. Inline-style declarations are the always-applied element-local layer,
        // so they are written straight onto the element's resolved style; stylesheet cascade layers
        // under them. Bindings and handlers are stored on the element's Bindings map keyed by the
        // bound attribute name and the event name respectively (an event name never collides with a
        // bound attribute), unresolved for the binding/handler resolution step.
        void PopulateElement(Element& element, const UIElementRecipe& recipe)
        {
            element.Id = recipe.Id;
            element.Classes = recipe.Classes;
            element.Text = recipe.Text;

            for (const StyleDeclaration& declaration : recipe.InlineStyle)
            {
                ApplyDeclaration(element.ComputedStyle, declaration, {});
            }

            for (const UIBindingRecipe& binding : recipe.Bindings)
            {
                element.Bindings[binding.Property] = binding.Expression;
            }
            for (const UIHandlerRecipe& handler : recipe.Handlers)
            {
                element.Bindings[handler.Event] = handler.Handler;
            }
        }
    }

    Unique<Document> Document::Instantiate(const UIDocument& recipe)
    {
        auto document = CreateUnique<Document>();

        const vector<UIElementRecipe>& elements = recipe.GetElements();
        if (elements.empty())
        {
            return document;
        }

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
            for (u32 i = 0; i < node.ChildCount; ++i)
            {
                Element& child = document->Add(live, elements[cursor].Kind);
                self(child, self);
            }
        };
        build(document->Root(), build);

        return document;
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
        Element& element = *owned;
        m_Elements.push_back(std::move(owned));

        const YGNodeRef node = m_Yoga->Create(element);
        if (kind == ElementKind::Text)
        {
            YGNodeSetMeasureFunc(node, &MeasureText);
        }
        return element;
    }

    Element& Document::Add(Element& parent, ElementKind kind)
    {
        Element& child = CreateElement(kind);
        child.Parent = &parent;
        parent.Children.push_back(&child);

        const YGNodeRef parentNode = m_Yoga->Get(parent);
        const YGNodeRef childNode = m_Yoga->Get(child);
        VE_ASSERT(parentNode != nullptr && childNode != nullptr,
                  "Gui::Document::Add: parent element does not belong to this document");
        YGNodeInsertChild(parentNode, childNode, YGNodeGetChildCount(parentNode));

        m_Dirty = true;
        return child;
    }

    void Document::DestroySubtree(Element& element)
    {
        // Depth-first so a child's node is freed before its parent's.
        for (Element* child : element.Children)
        {
            DestroySubtree(*child);
        }

        m_Yoga->Destroy(element);

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
        element.ComputedStyle = style;
        if (const YGNodeRef node = m_Yoga->Get(element);
            node != nullptr && YGNodeHasMeasureFunc(node))
        {
            YGNodeMarkDirty(node);
        }
        m_Dirty = true;
    }

    void Document::SetTextMeasurer(TextMeasurer measurer)
    {
        m_Measurer = std::move(measurer);
        for (const Unique<Element>& owned : m_Elements)
        {
            if (owned->Kind != ElementKind::Text)
            {
                continue;
            }
            if (const YGNodeRef node = m_Yoga->Get(*owned); node != nullptr)
            {
                YGNodeMarkDirty(node);
            }
        }
        m_Dirty = true;
    }

    vec2 Document::MeasureElementText(const Element& element, optional<f32> availableWidth) const
    {
        if (m_Measurer)
        {
            return m_Measurer(element.Text, element.ComputedStyle, availableWidth);
        }

        const Style& style = element.ComputedStyle;
        if (!style.TextFont.IsLoaded() || element.Text.empty())
        {
            return vec2(0.0f);
        }

        const vector<u32> codepoints = DecodeUtf8(element.Text);
        const ShapeResult shaped =
            style.TextFont.Get()->ShapeRun(codepoints, style.TextSize, availableWidth);
        return shaped.Size;
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
        ApplyEdgeInsets(node, style.Padding, &YGNodeStyleSetPadding);

        YGNodeStyleSetPositionType(node, style.Position == PositionType::Absolute
                                             ? YGPositionTypeAbsolute
                                             : YGPositionTypeRelative);
        if (style.Position == PositionType::Absolute)
        {
            ApplyEdgeInsets(node, style.Inset, &YGNodeStyleSetPosition);
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
        const vec2 absoluteMin = origin + localMin;
        element.Layout = Rect{
            .Min = absoluteMin,
            .Size = vec2(YGNodeLayoutGetWidth(node), YGNodeLayoutGetHeight(node)),
        };

        for (Element* child : element.Children)
        {
            ReadLayout(*child, absoluteMin);
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

        ReadLayout(*m_Root, vec2(0.0f));

        m_Dirty = false;
        m_LastAvailable = available;
    }

    void Document::BuildElement(const Element& element, DrawList& list) const
    {
        if (!element.Visible)
        {
            return;
        }

        const Style& style = element.ComputedStyle;
        const Rect& rect = element.Layout;

        if (style.Background.a > 0.0f || style.BorderStyle.Width > 0.0f)
        {
            if (style.Background.a > 0.0f)
            {
                list.Quad(rect, style.Background, style.Radii);
            }
            if (style.BorderStyle.Width > 0.0f)
            {
                list.Quad(rect, style.BorderStyle.Color, style.Radii, style.BorderStyle);
            }
        }

        if (style.ClipContent)
        {
            list.PushClip(rect);
        }

        if (element.Kind == ElementKind::Text && !element.Text.empty() && style.TextFont.IsLoaded())
        {
            list.Text(rect.Min, *style.TextFont.Get(), element.Text, style.TextSize,
                      style.TextColor);
        }

        for (const Element* child : element.Children)
        {
            BuildElement(*child, list);
        }

        if (style.ClipContent)
        {
            list.PopClip();
        }
    }

    void Document::Build(DrawList& list) const
    {
        BuildElement(*m_Root, list);
    }
}
