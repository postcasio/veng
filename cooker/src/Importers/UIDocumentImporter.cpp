#include "UIDocumentImporter.h"

#include "StyleParse.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <array>
#include <charconv>
#include <cstring>
#include <map>

#include <fmt/format.h>
#include <pugixml.hpp>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/StyleProperty.h>

namespace Veng::Cook
{
    namespace
    {
        optional<Gui::ElementKind> ParseElementKind(std::string_view tag)
        {
            if (tag == "Panel")
            {
                return Gui::ElementKind::Panel;
            }
            if (tag == "Text")
            {
                return Gui::ElementKind::Text;
            }
            if (tag == "Image")
            {
                return Gui::ElementKind::Image;
            }
            if (tag == "Button")
            {
                return Gui::ElementKind::Button;
            }
            if (tag == "Checkbox")
            {
                return Gui::ElementKind::Checkbox;
            }
            if (tag == "Slider")
            {
                return Gui::ElementKind::Slider;
            }
            if (tag == "ProgressBar")
            {
                return Gui::ElementKind::ProgressBar;
            }
            if (tag == "TextInput")
            {
                return Gui::ElementKind::TextInput;
            }
            if (tag == "ScrollView")
            {
                return Gui::ElementKind::ScrollView;
            }
            if (tag == "List")
            {
                return Gui::ElementKind::List;
            }
            if (tag == "Table")
            {
                return Gui::ElementKind::Table;
            }
            return std::nullopt;
        }

        // A growing UTF-8 string region with byte-offset deduplication: an identical string is
        // stored once and returns the same span, so a repeated class/id appears once in the blob.
        struct StringPool
        {
            vector<u8> Bytes;
            std::map<string, CookedUIStringSpan> Interned;

            CookedUIStringSpan Add(const string& text)
            {
                if (text.empty())
                {
                    return CookedUIStringSpan{.Offset = 0, .Length = 0};
                }
                const auto it = Interned.find(text);
                if (it != Interned.end())
                {
                    return it->second;
                }
                const CookedUIStringSpan span{.Offset = static_cast<u32>(Bytes.size()),
                                              .Length = static_cast<u32>(text.size())};
                Bytes.insert(Bytes.end(), text.begin(), text.end());
                Interned.emplace(text, span);
                return span;
            }
        };

        template <class T>
        void Append(vector<u8>& out, const T& value)
        {
            const auto* p = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), p, p + sizeof(T));
        }

        // The accumulated cook state: the flat pre-order tables assembled during the tree walk.
        struct Build
        {
            StringPool Strings;
            vector<CookedUIElement> Elements;
            vector<CookedUIStringSpan> Classes;
            vector<CookedUIBinding> Bindings;
            vector<CookedUIHandler> Handlers;
            vector<CookedStyleProperty> InlineProperties;
        };

        vector<string> SplitWhitespace(std::string_view text)
        {
            vector<string> parts;
            usize i = 0;
            const usize n = text.size();
            while (i < n)
            {
                while (i < n && std::isspace(static_cast<unsigned char>(text[i])) != 0)
                {
                    ++i;
                }
                const usize start = i;
                while (i < n && std::isspace(static_cast<unsigned char>(text[i])) == 0)
                {
                    ++i;
                }
                if (i > start)
                {
                    parts.emplace_back(text.substr(start, i - start));
                }
            }
            return parts;
        }

        // The active markup-repetition context threaded through the element cook: whether the
        // element sits inside a `count` subtree and, if so, its 0-based repeat index. `${}`
        // substitution is enabled only while Active, and a `count` seen while Active is a nested
        // repeat (a located error).
        struct RepeatContext
        {
            bool Active = false;
            u32 Index = 0;
        };

        // Evaluates one `${…}` body against the repeat index: `i` (0-based), `n` (1-based), each
        // optionally with a `:0W` zero-pad spec (W a single digit 1–9). An unknown name or a
        // malformed pad spec is a located error.
        Result<string> EvalSubstitution(std::string_view body, u32 index, const string& located)
        {
            const usize colon = body.find(':');
            const std::string_view name = body.substr(0, colon);

            u32 value = 0;
            if (name == "i")
            {
                value = index;
            }
            else if (name == "n")
            {
                value = index + 1;
            }
            else
            {
                return std::unexpected(fmt::format(
                    "{}: '${{{}}}' names an unknown index (expected 'i' or 'n')", located, body));
            }

            if (colon == std::string_view::npos)
            {
                return fmt::format("{}", value);
            }

            const std::string_view spec = body.substr(colon + 1);
            if (spec.size() != 2 || spec[0] != '0' || spec[1] < '1' || spec[1] > '9')
            {
                return std::unexpected(
                    fmt::format("{}: '${{{}}}' has a malformed pad spec (expected ':0W', W a digit "
                                "1–9, e.g. ':02')",
                                located, body));
            }
            const int width = spec[1] - '0';
            return fmt::format("{:0{}}", value, width);
        }

        // Applies `${}` index substitution to one attribute value or text run. Runs on every value
        // after XML parse and before attribute interpretation. `$${` emits a literal `${`; a `$`
        // not opening `${` passes through. A `${…}` form substitutes the repeat index while
        // `ctx.Active`; encountered outside a repeat subtree it is a located error (a typo-catch),
        // as is an unterminated `${`.
        Result<string> Substitute(std::string_view value, const RepeatContext& ctx,
                                  const string& located)
        {
            string out;
            usize i = 0;
            const usize n = value.size();
            while (i < n)
            {
                if (value[i] != '$')
                {
                    out.push_back(value[i]);
                    ++i;
                    continue;
                }
                if (i + 2 < n && value[i + 1] == '$' && value[i + 2] == '{')
                {
                    out += "${";
                    i += 3;
                    continue;
                }
                if (i + 1 < n && value[i + 1] == '{')
                {
                    if (!ctx.Active)
                    {
                        return std::unexpected(fmt::format("{}: index substitution '${{' appears "
                                                           "outside a 'count' subtree in '{}'",
                                                           located, value));
                    }
                    const usize close = value.find('}', i + 2);
                    if (close == std::string_view::npos)
                    {
                        return std::unexpected(
                            fmt::format("{}: unterminated '${{' in '{}'", located, value));
                    }
                    const Result<string> replaced =
                        EvalSubstitution(value.substr(i + 2, close - (i + 2)), ctx.Index, located);
                    if (!replaced)
                    {
                        return std::unexpected(replaced.error());
                    }
                    out += *replaced;
                    i = close + 1;
                    continue;
                }
                out.push_back('$');
                ++i;
            }
            return out;
        }

        // Parses a `count` attribute value: a plain integer in [1, 1024] (the ceiling is a
        // copy-paste guard, not a budget). A non-integer value or one out of range is a located
        // error.
        Result<u32> ParseCount(std::string_view value, const string& located)
        {
            std::string_view trimmed = value;
            while (!trimmed.empty() &&
                   std::isspace(static_cast<unsigned char>(trimmed.front())) != 0)
            {
                trimmed.remove_prefix(1);
            }
            while (!trimmed.empty() &&
                   std::isspace(static_cast<unsigned char>(trimmed.back())) != 0)
            {
                trimmed.remove_suffix(1);
            }
            long parsed = 0;
            const auto [ptr, ec] =
                std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed);
            if (ec != std::errc{} || ptr != trimmed.data() + trimmed.size())
            {
                return std::unexpected(
                    fmt::format("{}: 'count' value '{}' is not an integer", located, value));
            }
            if (parsed < 1 || parsed > 1024)
            {
                return std::unexpected(
                    fmt::format("{}: 'count' value {} is out of range [1, 1024]", located, parsed));
            }
            return static_cast<u32>(parsed);
        }

        // Parses an Image `uv` attribute — four space-separated floats {minX, minY, sizeX, sizeY},
        // the UV sub-rect the element samples — into that order. A wrong count or a non-numeric
        // component is a located error. Uses from_chars: the cooker builds -fno-exceptions.
        Result<std::array<f32, 4>> ParseUvRect(std::string_view value, const string& located)
        {
            const vector<string> parts = SplitWhitespace(value);
            if (parts.size() != 4)
            {
                return std::unexpected(fmt::format("{}: 'uv' expects four space-separated numbers "
                                                   "{{minX minY sizeX sizeY}}, got '{}'",
                                                   located, value));
            }
            std::array<f32, 4> uv{};
            for (usize i = 0; i < 4; ++i)
            {
                const string& part = parts[i];
                const auto [ptr, ec] =
                    std::from_chars(part.data(), part.data() + part.size(), uv[i]);
                if (ec != std::errc{} || ptr != part.data() + part.size())
                {
                    return std::unexpected(
                        fmt::format("{}: 'uv' component '{}' is not a number", located, part));
                }
            }
            return uv;
        }

        // Parses an inline `style="prop: value; …"` attribute into cooked declarations.
        Result<vector<CookedStyleProperty>> ParseInlineStyle(std::string_view style,
                                                             const string& located)
        {
            vector<CookedStyleProperty> properties;
            usize i = 0;
            const usize n = style.size();
            while (i < n)
            {
                // one `property: value` up to `;`
                const usize declStart = i;
                while (i < n && style[i] != ';')
                {
                    ++i;
                }
                const std::string_view decl = style.substr(declStart, i - declStart);
                if (i < n)
                {
                    ++i; // consume ';'
                }

                const usize colon = decl.find(':');
                if (colon == std::string_view::npos)
                {
                    // A trailing empty segment (e.g. after the final ';') is skipped.
                    bool blank = true;
                    for (const char c : decl)
                    {
                        if (std::isspace(static_cast<unsigned char>(c)) == 0)
                        {
                            blank = false;
                            break;
                        }
                    }
                    if (blank)
                    {
                        continue;
                    }
                    return std::unexpected(fmt::format(
                        "{}: inline style declaration '{}' is missing a ':'", located, decl));
                }

                std::string_view name = decl.substr(0, colon);
                while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())) != 0)
                {
                    name.remove_prefix(1);
                }
                while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())) != 0)
                {
                    name.remove_suffix(1);
                }
                const std::string_view value = decl.substr(colon + 1);

                const optional<Gui::StyleProperty> property = Gui::ParseStyleProperty(string(name));
                if (!property)
                {
                    return std::unexpected(
                        fmt::format("{}: unknown style property '{}'", located, name));
                }
                // The box-shadow shorthand cooks to two declarations (geometry and color), so it
                // cannot ride the one-declaration return the other properties share.
                if (*property == Gui::StyleProperty::BoxShadow)
                {
                    const Result<vector<CookedStyleProperty>> shadow =
                        ParseBoxShadowDeclaration(value, located);
                    if (!shadow)
                    {
                        return std::unexpected(shadow.error());
                    }
                    properties.insert(properties.end(), shadow->begin(), shadow->end());
                    continue;
                }

                const Result<CookedStyleProperty> cooked =
                    ParseStyleDeclaration(*property, value, located);
                if (!cooked)
                {
                    return std::unexpected(cooked.error());
                }
                properties.push_back(*cooked);
            }
            if (const VoidResult exclusive = CheckExclusiveFillSources(properties, located);
                !exclusive)
            {
                return std::unexpected(exclusive.error());
            }
            return properties;
        }

        // Recursively cooks one XML element into the flat tables, returning its recursive subtree
        // node count (so a parent counts only its direct children). The element's own record is
        // appended first (pre-order), then its children follow. `subst` carries the active
        // repetition context: while Active, every attribute value and text run has `${}` index
        // substitution applied, and a child bearing `count` (a nested repeat) is a located error.
        Result<u32> CookElement(const pugi::xml_node& node, Build& build, const string& file,
                                const RepeatContext& subst)
        {
            const string tag = node.name();
            const optional<Gui::ElementKind> kind = ParseElementKind(tag);
            if (!kind)
            {
                return std::unexpected(
                    fmt::format("ui document importer: '{}': unknown element tag '{}'", file, tag));
            }

            CookedUIElement element{};
            element.Kind = static_cast<u32>(*kind);

            const string located = fmt::format("ui document importer: '{}': <{}>", file, tag);

            element.FirstClass = static_cast<u32>(build.Classes.size());
            element.FirstBinding = static_cast<u32>(build.Bindings.size());
            element.FirstHandler = static_cast<u32>(build.Handlers.size());
            element.FirstInlineProperty = static_cast<u32>(build.InlineProperties.size());

            for (const pugi::xml_attribute& attr : node.attributes())
            {
                const string name = attr.name();

                // `count` is consumed by the caller's repetition handling; it is never an element
                // attribute and never emitted.
                if (name == "count")
                {
                    continue;
                }

                // Index substitution runs on the raw value before any attribute interpretation, so
                // a substituted id/class/style/binding is then processed exactly as if hand-typed.
                const Result<string> substituted = Substitute(attr.value(), subst, located);
                if (!substituted)
                {
                    return std::unexpected(substituted.error());
                }
                const string value = *substituted;

                if (name == "id")
                {
                    element.Id = build.Strings.Add(value);
                    continue;
                }
                if (name == "class")
                {
                    for (const string& tagName : SplitWhitespace(value))
                    {
                        build.Classes.push_back(build.Strings.Add(tagName));
                    }
                    continue;
                }
                if (name == "style")
                {
                    const Result<vector<CookedStyleProperty>> inline_ =
                        ParseInlineStyle(value, located);
                    if (!inline_)
                    {
                        return std::unexpected(inline_.error());
                    }
                    for (const CookedStyleProperty& cp : *inline_)
                    {
                        build.InlineProperties.push_back(cp);
                    }
                    continue;
                }

                // A `{expr}` value is a binding; an `on*` attribute is a named handler; anything
                // else is an unrecognized attribute (a typo-catch, not silently ignored).
                if (value.size() >= 2 && value.front() == '{' && value.back() == '}')
                {
                    const string expression = value.substr(1, value.size() - 2);
                    CookedUIBinding binding{};
                    binding.Property = build.Strings.Add(name);
                    binding.Expression = build.Strings.Add(expression);
                    build.Bindings.push_back(binding);
                    continue;
                }
                if (name.size() > 2 && name[0] == 'o' && name[1] == 'n')
                {
                    CookedUIHandler handler{};
                    handler.Event = build.Strings.Add(name);
                    handler.Handler = build.Strings.Add(value);
                    build.Handlers.push_back(handler);
                    continue;
                }

                // A closed set of widget-config attributes carries a literal (not a `{binding}`):
                // a control's range/step, its initial value, a Slider's orientation, and an item
                // host's selection mode. They are stored on the element's binding table verbatim;
                // the runtime widget layer reads them at instantiate time.
                if (name == "min" || name == "max" || name == "step" || name == "value" ||
                    name == "checked" || name == "items" || name == "orientation" ||
                    name == "selection")
                {
                    CookedUIBinding binding{};
                    binding.Property = build.Strings.Add(name);
                    binding.Expression = build.Strings.Add(value);
                    build.Bindings.push_back(binding);
                    continue;
                }

                // An Image carries a source texture (`src`), an optional tint, and an optional UV
                // sub-rect as literal attributes. The `src` id is written onto the element and
                // becomes a document texture dependency (the loader eager-loads it, resident, as it
                // does a stylesheet's fonts); the tint/uv fold onto the element with the defaults
                // (opaque white / the whole texture) when absent.
                if (*kind == Gui::ElementKind::Image &&
                    (name == "src" || name == "tint" || name == "uv"))
                {
                    if (name == "src")
                    {
                        const optional<AssetId> texId = ParseAssetId(value);
                        if (!texId)
                        {
                            return std::unexpected(fmt::format(
                                "{}: 'src' value '{}' is not a hex AssetId", located, value));
                        }
                        element.Src = texId->Value;
                    }
                    else if (name == "tint")
                    {
                        const Result<vec4> tint = ParseStyleColor(value, located);
                        if (!tint)
                        {
                            return std::unexpected(tint.error());
                        }
                        element.Tint[0] = tint->r;
                        element.Tint[1] = tint->g;
                        element.Tint[2] = tint->b;
                        element.Tint[3] = tint->a;
                    }
                    else
                    {
                        const Result<std::array<f32, 4>> uv = ParseUvRect(value, located);
                        if (!uv)
                        {
                            return std::unexpected(uv.error());
                        }
                        element.Uv[0] = (*uv)[0];
                        element.Uv[1] = (*uv)[1];
                        element.Uv[2] = (*uv)[2];
                        element.Uv[3] = (*uv)[3];
                    }
                    continue;
                }

                return std::unexpected(fmt::format("{}: unrecognized attribute '{}' (expected "
                                                   "id/class/style, a widget-config attribute, a "
                                                   "{{binding}}, or an on* handler)",
                                                   located, name));
            }

            // A Text element's content is its direct text (child text nodes concatenated, trimmed).
            if (*kind == Gui::ElementKind::Text)
            {
                string text;
                for (const pugi::xml_node& child : node.children())
                {
                    if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata)
                    {
                        text += child.value();
                    }
                }
                usize begin = 0;
                usize end = text.size();
                while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0)
                {
                    ++begin;
                }
                while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
                {
                    --end;
                }
                const Result<string> substituted =
                    Substitute(text.substr(begin, end - begin), subst, located);
                if (!substituted)
                {
                    return std::unexpected(substituted.error());
                }
                element.Text = build.Strings.Add(*substituted);
            }
            else
            {
                // A Button (and other kinds) may carry inline label text as its direct text.
                string text;
                for (const pugi::xml_node& child : node.children())
                {
                    if (child.type() == pugi::node_pcdata)
                    {
                        text += child.value();
                    }
                }
                usize begin = 0;
                usize end = text.size();
                while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0)
                {
                    ++begin;
                }
                while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
                {
                    --end;
                }
                if (end > begin)
                {
                    const Result<string> substituted =
                        Substitute(text.substr(begin, end - begin), subst, located);
                    if (!substituted)
                    {
                        return std::unexpected(substituted.error());
                    }
                    element.Text = build.Strings.Add(*substituted);
                }
            }

            element.ClassCount = static_cast<u32>(build.Classes.size()) - element.FirstClass;
            element.BindingCount = static_cast<u32>(build.Bindings.size()) - element.FirstBinding;
            element.HandlerCount = static_cast<u32>(build.Handlers.size()) - element.FirstHandler;
            element.InlinePropertyCount =
                static_cast<u32>(build.InlineProperties.size()) - element.FirstInlineProperty;

            // Reserve this element's slot before recursing so children follow it in pre-order.
            const usize selfIndex = build.Elements.size();
            build.Elements.push_back(element);

            u32 childCount = 0;
            for (const pugi::xml_node& child : node.children())
            {
                if (child.type() != pugi::node_element)
                {
                    continue;
                }

                // A `count` child unrolls in place: the same subtree cook is run N times, each
                // with its own repeat index. Absent, the child inherits this element's repeat
                // context (so an element inside a repeated subtree keeps that subtree's index).
                if (const pugi::xml_attribute countAttr = child.attribute("count"))
                {
                    const string childLocated =
                        fmt::format("ui document importer: '{}': <{}>", file, child.name());
                    if (subst.Active)
                    {
                        return std::unexpected(fmt::format(
                            "{}: nested 'count' — an element with 'count' already sits inside a "
                            "repeated subtree",
                            childLocated));
                    }
                    const Result<u32> repeats = ParseCount(countAttr.value(), childLocated);
                    if (!repeats)
                    {
                        return std::unexpected(repeats.error());
                    }
                    for (u32 index = 0; index < *repeats; ++index)
                    {
                        const Result<u32> subtree = CookElement(
                            child, build, file, RepeatContext{.Active = true, .Index = index});
                        if (!subtree)
                        {
                            return std::unexpected(subtree.error());
                        }
                        ++childCount;
                    }
                    continue;
                }

                const Result<u32> subtree = CookElement(child, build, file, subst);
                if (!subtree)
                {
                    return std::unexpected(subtree.error());
                }
                ++childCount;
            }

            build.Elements[selfIndex].ChildCount = childCount;
            return childCount + 1;
        }
    }

    Result<vector<u8>> UIDocumentImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("ui document importer: missing or invalid 'source'");
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();
        const string file = sourcePath.string();

        pugi::xml_document doc;
        const pugi::xml_parse_result parsed = doc.load_file(sourcePath.string().c_str());
        if (!parsed)
        {
            return std::unexpected(
                fmt::format("ui document importer: '{}': XML parse error at offset {}: {}", file,
                            static_cast<long long>(parsed.offset), parsed.description()));
        }

        pugi::xml_node root = doc.first_child();
        if (!root || root.type() != pugi::node_element)
        {
            return std::unexpected(
                fmt::format("ui document importer: '{}': document has no root element", file));
        }

        // The root's `stylesheets` attribute lists the referenced StyleSheet ids (hex, space
        // separated); it is consumed here, not passed to the element cook.
        vector<u64> styleSheetIds;
        if (const pugi::xml_attribute sheets = root.attribute("stylesheets"))
        {
            for (const string& idText : SplitWhitespace(sheets.value()))
            {
                const optional<AssetId> id = ParseAssetId(idText);
                if (!id)
                {
                    return std::unexpected(fmt::format(
                        "ui document importer: '{}': 'stylesheets' entry '{}' is not a hex AssetId",
                        file, idText));
                }
                styleSheetIds.push_back(id->Value);
            }
            // The attribute is not an element attribute; remove it so the element cook does not
            // reject it as unrecognized.
            root.remove_attribute("stylesheets");
        }

        // `count` on the root has no in-place sibling context to unroll into, and would make the
        // whole document its own repeated subtree — a located error, not a silent no-op.
        if (root.attribute("count"))
        {
            return std::unexpected(fmt::format(
                "ui document importer: '{}': 'count' is not allowed on the root element", file));
        }

        Build build;
        const Result<u32> rootResult = CookElement(root, build, file, RepeatContext{});
        if (!rootResult)
        {
            return std::unexpected(rootResult.error());
        }

        CookedUIDocumentHeader header{};
        header.Version = CookedUIDocumentVersion;
        header.StyleSheetCount = static_cast<u32>(styleSheetIds.size());
        header.ElementCount = static_cast<u32>(build.Elements.size());
        header.ClassCount = static_cast<u32>(build.Classes.size());
        header.BindingCount = static_cast<u32>(build.Bindings.size());
        header.HandlerCount = static_cast<u32>(build.Handlers.size());
        header.InlinePropertyCount = static_cast<u32>(build.InlineProperties.size());
        header.StringBytes = static_cast<u32>(build.Strings.Bytes.size());

        vector<u8> blob;
        Append(blob, header);
        for (const u64 id : styleSheetIds)
        {
            Append(blob, id);
        }
        for (const CookedUIElement& element : build.Elements)
        {
            Append(blob, element);
        }
        for (const CookedUIStringSpan& span : build.Classes)
        {
            Append(blob, span);
        }
        for (const CookedUIBinding& binding : build.Bindings)
        {
            Append(blob, binding);
        }
        for (const CookedUIHandler& handler : build.Handlers)
        {
            Append(blob, handler);
        }
        for (const CookedStyleProperty& cp : build.InlineProperties)
        {
            Append(blob, cp);
        }
        blob.insert(blob.end(), build.Strings.Bytes.begin(), build.Strings.Bytes.end());

        return blob;
    }

    void RegisterUIDocumentImporter(Cooker& cooker)
    {
        cooker.Register(CreateUnique<UIDocumentImporter>());
    }
}
