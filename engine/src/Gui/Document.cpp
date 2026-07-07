#include <Veng/Gui/Document.h>

#include "YogaTree.h"

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Font.h>
#include <Veng/Gui/StyleSheet.h>
#include <Veng/Gui/UIDocument.h>
#include <Veng/Reflection/EnumName.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Viewport.h>

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cmath>

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
            }
            return "Panel";
        }

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
        // cascade's last-wins), so the live document never borrows the sheet.
        void ResolveElementStyle(Element& element, const UIElementRecipe& recipe,
                                 const vector<const StyleSheet*>& sheets, const FontResolver& fonts)
        {
            element.Variants.clear();
            element.Animations.clear();

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
                            ApplyDeclaration(element.BaseStyle, declaration, fonts);
                        }
                    }
                    else
                    {
                        element.Variants.push_back(
                            StyleVariant{.State = rule.State, .Declarations = rule.Declarations});
                    }
                }
            }

            for (const StyleDeclaration& declaration : recipe.InlineStyle)
            {
                ApplyDeclaration(element.BaseStyle, declaration, fonts);
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

    Unique<Document> Document::Instantiate(const UIDocument& recipe, const FontResolver& fonts)
    {
        auto document = CreateUnique<Document>();
        document->m_FontResolver = fonts;

        const vector<UIElementRecipe>& elements = recipe.GetElements();
        if (elements.empty())
        {
            return document;
        }

        vector<const StyleSheet*> sheets;
        sheets.reserve(recipe.GetStyleSheets().size());
        for (const AssetHandle<StyleSheet>& handle : recipe.GetStyleSheets())
        {
            if (handle.IsLoaded())
            {
                sheets.push_back(handle.Get());
            }
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
            ResolveElementStyle(live, node, sheets, fonts);
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

    Unique<Document> Document::Instantiate(const UIDocument& recipe, AssetManager& assets)
    {
        return Instantiate(recipe, [&assets](const AssetId id)
                           { return assets.LoadSync<Font>(id).value_or(AssetHandle<Font>{}); });
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
        // Text and Button are label leaves: their intrinsic size is their shaped text, so both
        // carry the text measure. (A measured node cannot take children — Yoga asserts loudly.)
        if (kind == ElementKind::Text || kind == ElementKind::Button)
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

        // A Button is a label leaf (text-measured) until it takes a child, at which point it
        // becomes a container sized by its children like a Panel — a measured Yoga node cannot
        // hold children. Text stays a hard leaf.
        if (parent.Kind == ElementKind::Button && YGNodeHasMeasureFunc(parentNode))
        {
            YGNodeSetMeasureFunc(parentNode, nullptr);
        }
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
        element.BaseStyle = style;
        element.ComputedStyle = style;
        element.Tweens.clear();
        if (const YGNodeRef node = m_Yoga->Get(element);
            node != nullptr && YGNodeHasMeasureFunc(node))
        {
            YGNodeMarkDirty(node);
        }
        m_Dirty = true;
    }

    void Document::SetOpacity(Element& element, const f32 opacity)
    {
        element.BaseStyle.Opacity = opacity;
        element.ComputedStyle.Opacity = opacity;
    }

    void Document::SetBackground(Element& element, const vec4 color)
    {
        element.BaseStyle.Background = color;
        element.ComputedStyle.Background = color;
    }

    void Document::SetTextColor(Element& element, const vec4 color)
    {
        element.BaseStyle.TextColor = color;
        element.ComputedStyle.TextColor = color;
    }

    void Document::SetPlacement(Element& element, const vec2 topLeft, const vec2 size)
    {
        Style& base = element.BaseStyle;
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
            case StyleProperty::TextSize:
            case StyleProperty::TextFont:
                return true;
            case StyleProperty::Background:
            case StyleProperty::CornerRadius:
            case StyleProperty::BorderWidth:
            case StyleProperty::BorderColor:
            case StyleProperty::TextColor:
            case StyleProperty::Opacity:
            case StyleProperty::ClipContent:
            case StyleProperty::PointerEvents:
            case StyleProperty::Animation:
                return false;
            }
            return false;
        }

        // Whether a property's value type interpolates continuously: colors, scalars, corner radii,
        // edge insets, and same-kind lengths. Enums, fonts, and the clip flag snap.
        bool IsAnimatableProperty(StyleProperty property)
        {
            switch (property)
            {
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
            case StyleProperty::Inset:
            case StyleProperty::InsetLeft:
            case StyleProperty::InsetTop:
            case StyleProperty::InsetRight:
            case StyleProperty::InsetBottom:
            case StyleProperty::Background:
            case StyleProperty::CornerRadius:
            case StyleProperty::BorderWidth:
            case StyleProperty::BorderColor:
            case StyleProperty::TextColor:
            case StyleProperty::TextSize:
            case StyleProperty::Opacity:
                return true;
            case StyleProperty::FlexDirection:
            case StyleProperty::JustifyContent:
            case StyleProperty::AlignItems:
            case StyleProperty::AlignSelf:
            case StyleProperty::FlexWrap:
            case StyleProperty::Position:
            case StyleProperty::TextFont:
            case StyleProperty::ClipContent:
            case StyleProperty::PointerEvents:
            case StyleProperty::Animation:
                return false;
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
            case StyleProperty::InsetLeft:
                return vec4(style.Inset.Left, 0.0f, 0.0f, 0.0f);
            case StyleProperty::InsetTop:
                return vec4(style.Inset.Top, 0.0f, 0.0f, 0.0f);
            case StyleProperty::InsetRight:
                return vec4(style.Inset.Right, 0.0f, 0.0f, 0.0f);
            case StyleProperty::InsetBottom:
                return vec4(style.Inset.Bottom, 0.0f, 0.0f, 0.0f);
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
            default:
                return;
            }
        }

        // Folds the variants whose state bit is set in `state` over the base style, in stored source
        // order (later-listed states win — the USS order), producing the resolved target style.
        Style ResolveTarget(const Element& element, const FontResolver& fonts)
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
                    ApplyDeclaration(target, declaration, fonts);
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
                                   const FontResolver& fonts)
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
                ApplyDeclaration(live, before != nullptr ? *before : *after, fonts);
                return;
            }
            if (!IsAnimatableProperty(property) || before->Unit != after->Unit)
            {
                ApplyDeclaration(live, *before, fonts);
                return;
            }

            const f32 span = afterOffset - beforeOffset;
            const f32 u = span > 0.0f ? (phase - beforeOffset) / span : 1.0f;
            StyleDeclaration blended = *before;
            blended.Values = glm::mix(before->Values, after->Values, u);
            ApplyDeclaration(live, blended, fonts);
        }

        // Applies one animation's keyframes at its current phase onto the live style, resolving
        // each declared property once across the whole clip.
        void ApplyAnimation(Style& live, const StyleAnimation& animation, const FontResolver& fonts)
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
                    ApplyAnimatedProperty(live, animation, declaration.Property, phase, fonts);
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
        const Style target = ResolveTarget(element, m_FontResolver);

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
            ApplyAnimation(live, animation, m_FontResolver);
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
        if (element.ComputedStyle.TextFont.Id().Value != live.TextFont.Id().Value)
        {
            layoutMoved = true;
        }

        element.ComputedStyle = live;

        if (layoutMoved)
        {
            m_Dirty = true;
            if (const YGNodeRef node = m_Yoga->Get(element);
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

    namespace
    {
        // Whether an element kind is a focusable, interactive control — the widget layer sets
        // Element::Focusable on these so directional/Tab navigation and Confirm reach them, and a
        // plain Panel/Text/Image stays a non-stop.
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

        if (changed && element.Kind != ElementKind::ProgressBar)
        {
            static_cast<void>(FireHandler(element, "onChange"));
        }
    }

    void Document::ScrollBy(Element& element, vec2 delta)
    {
        if (element.Kind != ElementKind::ScrollView)
        {
            return;
        }

        // The scrollable extent is how far the content overflows the viewport; clamp so the offset
        // never scrolls the content past its own bounds.
        vec2 content(0.0f);
        for (const Element* child : element.Children)
        {
            content = glm::max(content, child->Layout.Max() - element.Layout.Min);
        }
        const vec2 overflow = glm::max(content - element.Layout.Size, vec2(0.0f));

        const vec2 next = glm::clamp(element.Widget.ScrollOffset + delta, vec2(0.0f), overflow);
        if (next != element.Widget.ScrollOffset)
        {
            element.Widget.ScrollOffset = next;
            m_Dirty = true;
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

        if (element.Kind == ElementKind::Slider)
        {
            element.Widget.Min = ReadConfigScalar(element, "min", 0.0f);
            element.Widget.Max = ReadConfigScalar(element, "max", 1.0f);
            element.Widget.Step = ReadConfigScalar(element, "step", 0.0f);
            element.Widget.Value =
                ClampStep(ReadConfigScalar(element, "value", element.Widget.Min),
                          element.Widget.Min, element.Widget.Max, element.Widget.Step);
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
    }

    bool Document::DriveWidgetPointer(Element& element, const PointerEvent& event)
    {
        if (element.Kind == ElementKind::Slider)
        {
            // A press or drag over the track sets the value from the pointer's fraction along the
            // slider's width — the pointer maps linearly from Min at the left edge to Max at the right.
            if (event.Kind != PointerEventKind::Down && event.Kind != PointerEventKind::Move)
            {
                return false;
            }
            if (event.Kind == PointerEventKind::Move && m_PressTarget != &element)
            {
                return false;
            }
            const f32 width = element.Layout.Size.x;
            if (width <= 0.0f)
            {
                return false;
            }
            const f32 fraction =
                std::clamp((event.Position.x - element.Layout.Min.x) / width, 0.0f, 1.0f);
            SetWidgetValue(element, element.Widget.Min +
                                        fraction * (element.Widget.Max - element.Widget.Min));
            return true;
        }

        if (element.Kind == ElementKind::ScrollView)
        {
            // A drag with the press captured on the scroll view pans its content by the pointer delta.
            if (event.Kind != PointerEventKind::Move || m_PressTarget != &element)
            {
                return false;
            }
            const vec2 delta = m_LastScrollPointer - event.Position;
            m_LastScrollPointer = event.Position;
            ScrollBy(element, delta);
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
            if (action == NavAction::MoveLeft)
            {
                SetWidgetValue(element, element.Widget.Value - step);
                return true;
            }
            if (action == NavAction::MoveRight)
            {
                SetWidgetValue(element, element.Widget.Value + step);
                return true;
            }
            return false;
        }

        if (element.Kind == ElementKind::ScrollView)
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
        const vec2 absoluteMin = origin + localMin;
        element.Layout = Rect{
            .Min = absoluteMin,
            .Size = vec2(YGNodeLayoutGetWidth(node), YGNodeLayoutGetHeight(node)),
        };

        // A ScrollView shifts its children by its scroll offset, so the child origin is the view's
        // top-left minus the offset — the content slides under the clip the ScrollView paints with.
        const vec2 childOrigin = element.Kind == ElementKind::ScrollView
                                     ? absoluteMin - element.Widget.ScrollOffset
                                     : absoluteMin;

        for (Element* child : element.Children)
        {
            ReadLayout(*child, childOrigin);
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

        // A ScrollView always clips its content to its box (the overflow it scrolls through), whether
        // or not the style requested it; every other kind clips only when styled to.
        const bool clip = style.ClipContent || element.Kind == ElementKind::ScrollView;
        if (clip)
        {
            list.PushClip(rect);
        }

        // A control paints its own parts (a Slider's track and thumb, a ProgressBar's fill) between
        // its background and its children.
        BuildWidget(element, list);

        if (!element.Text.empty() && style.TextFont.IsLoaded() &&
            (element.Kind == ElementKind::Text || element.Kind == ElementKind::Button))
        {
            // A Text leaf draws at its content-box origin (inside the border and padding, the box
            // the measure sized); a Button centers its label in its box.
            vec2 origin = rect.Min + vec2(style.BorderStyle.Width + style.Padding.Left,
                                          style.BorderStyle.Width + style.Padding.Top);
            if (element.Kind == ElementKind::Button)
            {
                const vec2 label = MeasureElementText(element, std::nullopt);
                origin = rect.Min + (rect.Size - label) * 0.5f;
            }
            list.Text(origin, *style.TextFont.Get(), element.Text, style.TextSize, style.TextColor);
        }

        for (const Element* child : element.Children)
        {
            BuildElement(*child, list);
        }

        if (clip)
        {
            list.PopClip();
        }
    }

    void Document::BuildWidget(const Element& element, DrawList& list) const
    {
        const Style& style = element.ComputedStyle;
        const Rect& rect = element.Layout;

        if (element.Kind == ElementKind::ProgressBar)
        {
            // The fill is the [0,1] value across the bar's width, drawn in the text color over the
            // background track. A bar with no explicit value renders empty.
            const f32 fraction = std::clamp(element.Widget.Value, 0.0f, 1.0f);
            if (fraction > 0.0f)
            {
                const Rect fill{.Min = rect.Min, .Size = vec2(rect.Size.x * fraction, rect.Size.y)};
                list.Quad(fill, style.TextColor, style.Radii);
            }
            return;
        }

        if (element.Kind == ElementKind::Slider)
        {
            const f32 range = element.Widget.Max - element.Widget.Min;
            const f32 fraction =
                range != 0.0f
                    ? std::clamp((element.Widget.Value - element.Widget.Min) / range, 0.0f, 1.0f)
                    : 0.0f;
            // The filled portion of the track shows the value; a square thumb marks the position.
            const Rect track{.Min = rect.Min, .Size = vec2(rect.Size.x * fraction, rect.Size.y)};
            if (fraction > 0.0f)
            {
                list.Quad(track, style.TextColor, style.Radii);
            }
            const f32 thumb = rect.Size.y;
            const f32 thumbX = rect.Min.x + fraction * std::max(rect.Size.x - thumb, 0.0f);
            list.Quad(Rect{.Min = vec2(thumbX, rect.Min.y), .Size = vec2(thumb, thumb)},
                      style.BorderStyle.Color.a > 0.0f ? style.BorderStyle.Color : style.TextColor,
                      style.Radii);
            return;
        }
    }

    void Document::Build(DrawList& list) const
    {
        BuildElement(*m_Root, list);
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
        if (element.ComputedStyle.ClipContent)
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

        if (Contains(element.Layout, point))
        {
            return &element;
        }
        return nullptr;
    }

    Element* Document::HitTest(vec2 point)
    {
        return HitTestElement(*m_Root, point, std::nullopt);
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
                    element.Widget.Caret = 0;
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

    bool Document::IsListItem(const Element& element) const
    {
        for (const Element* e = element.Parent; e != nullptr; e = e->Parent)
        {
            if (e->Kind == ElementKind::List)
            {
                return true;
            }
        }
        return false;
    }

    void Document::SyncLists()
    {
        // Collect the Lists first: SyncList mutates m_Elements (adding/removing item clones), so the
        // walk cannot iterate m_Elements directly.
        vector<Element*> lists;
        for (const Unique<Element>& element : m_Elements)
        {
            if (element->Kind == ElementKind::List)
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
            const vector<Element*> authored = list.Children;
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
            list.Children.clear();
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
        const usize haveItems = perItem == 0 ? 0 : list.Children.size() / perItem;

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
                Remove(*list.Children.back());
            }
        }

        // Resolve each item's per-item bindings against its array element.
        for (usize i = 0; i < count && perItem != 0; ++i)
        {
            void* itemPtr = field->Field->ArrayElement(field->Ptr, i);
            for (usize k = 0; k < perItem; ++k)
            {
                Element& itemRoot = *list.Children[i * perItem + k];
                ResolveItemBindings(itemRoot, itemPtr, field->Field->ElementType);
            }
        }
    }

    Element* Document::DetachTemplate(Element& element, vector<Unique<Element>>& owned)
    {
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
            // Display-only documents hold no hover/press/focus state; drop any live interaction.
            if (m_HoverTarget != nullptr)
            {
                SetState(*m_HoverTarget,
                         WithBit(m_HoverTarget->State, ElementState::Hovered, false));
                m_HoverTarget = nullptr;
            }
            if (m_PressTarget != nullptr)
            {
                SetState(*m_PressTarget,
                         WithBit(m_PressTarget->State, ElementState::Active, false));
                m_PressTarget = nullptr;
            }
            SetFocus(nullptr);
        }
    }

    bool Document::DispatchPointer(PointerEvent& event)
    {
        if (!m_Interactive)
        {
            return false;
        }

        Element* target = HitTest(event.Position);

        // Hover transitions: a move onto a new element leaves the old and enters the new, driving
        // the Hovered state a styling layer reads.
        if (target != m_HoverTarget)
        {
            if (m_HoverTarget != nullptr)
            {
                SetState(*m_HoverTarget,
                         WithBit(m_HoverTarget->State, ElementState::Hovered, false));
            }
            m_HoverTarget = target;
            if (m_HoverTarget != nullptr)
            {
                SetState(*m_HoverTarget,
                         WithBit(m_HoverTarget->State, ElementState::Hovered, true));
            }
        }

        event.Target = target;

        if (event.Kind == PointerEventKind::Down && target != nullptr)
        {
            // A ScrollView claims a press that lands on one of its children — the first-handler-wins
            // capture that lets a drag started over a list item pan the view rather than the item.
            Element* pressTarget = target;
            for (Element* e = target; e != nullptr; e = e->Parent)
            {
                if (e->Kind == ElementKind::ScrollView)
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
            m_LastScrollPointer = event.Position;
            SetState(*pressTarget, WithBit(pressTarget->State, ElementState::Active, true));
            if (pressTarget->Focusable)
            {
                SetFocus(pressTarget);
            }
            // A press on a Slider sets its value from where the pointer landed.
            static_cast<void>(DriveWidgetPointer(*pressTarget, event));
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
            // A press and its release on the same element is a click.
            if (m_PressTarget != nullptr && m_PressTarget == target)
            {
                PointerEvent click{.Kind = PointerEventKind::Click,
                                   .Button = event.Button,
                                   .Position = event.Position,
                                   .Target = target};
                RoutePointerPath(click);
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
        }

        return consumed || target != nullptr;
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

    bool Document::Navigate(NavAction action)
    {
        if (!m_Interactive)
        {
            return false;
        }

        if (action == NavAction::Confirm)
        {
            if (m_Focused == nullptr)
            {
                return false;
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
            return fired;
        }
        if (action == NavAction::Cancel)
        {
            return m_Focused != nullptr && FireHandler(*m_Focused, "onCancel");
        }

        // A directional action on a focused Slider nudges its value, and on a ScrollView scrolls it,
        // rather than moving focus off it.
        if (m_Focused != nullptr &&
            (m_Focused->Kind == ElementKind::Slider || m_Focused->Kind == ElementKind::ScrollView))
        {
            if (DriveWidgetNavigation(*m_Focused, action))
            {
                return true;
            }
        }

        vector<Element*> focusables;
        GatherFocusables(*m_Root, focusables);
        if (focusables.empty())
        {
            return false;
        }

        // With nothing focused, any navigation lands on the first focusable.
        if (m_Focused == nullptr)
        {
            SetFocus(focusables.front());
            return true;
        }

        if (action == NavAction::Next || action == NavAction::Previous)
        {
            const auto it = std::ranges::find(focusables, m_Focused);
            const usize index = it == focusables.end() ? 0 : usize(it - focusables.begin());
            const usize count = focusables.size();
            const usize next =
                action == NavAction::Next ? (index + 1) % count : (index + count - 1) % count;
            SetFocus(focusables[next]);
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
        SetFocus(best);
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
        for (Element* child : element.Children)
        {
            GatherFocusables(*child, out);
        }
    }
}
