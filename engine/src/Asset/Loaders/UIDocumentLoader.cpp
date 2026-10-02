#include "UIDocumentLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Font.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Gui/StyleSheet.h>

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }

        void AddUnique(vector<AssetId>& ids, AssetId id)
        {
            if (id.Value == 0)
            {
                return;
            }
            for (const AssetId existing : ids)
            {
                if (existing.Value == id.Value)
                {
                    return;
                }
            }
            ids.push_back(id);
        }
    }

    namespace Detail
    {
        AssetResult<DecodedUIDocument> DecodeUIDocument(AssetId id, std::span<const u8> cooked)
        {
            if (cooked.size() < sizeof(CookedUIDocumentHeader))
            {
                return std::unexpected(
                    Corrupt(id, "ui document: cooked blob smaller than CookedUIDocumentHeader"));
            }

            CookedUIDocumentHeader header;
            std::memcpy(&header, cooked.data(), sizeof(header));

            if (header.Version != CookedUIDocumentVersion)
            {
                return std::unexpected(Corrupt(
                    id,
                    fmt::format("ui document: blob version {} does not match expected version {}",
                                header.Version, CookedUIDocumentVersion)));
            }

            const usize styleSheetBytes = static_cast<usize>(header.StyleSheetCount) * sizeof(u64);
            const usize elementBytes =
                static_cast<usize>(header.ElementCount) * sizeof(CookedUIElement);
            const usize classBytes =
                static_cast<usize>(header.ClassCount) * sizeof(CookedUIStringSpan);
            const usize bindingBytes =
                static_cast<usize>(header.BindingCount) * sizeof(CookedUIBinding);
            const usize handlerBytes =
                static_cast<usize>(header.HandlerCount) * sizeof(CookedUIHandler);
            const usize inlineBytes =
                static_cast<usize>(header.InlinePropertyCount) * sizeof(CookedStyleProperty);

            usize cursor = sizeof(CookedUIDocumentHeader);
            const usize total = cursor + styleSheetBytes + elementBytes + classBytes +
                                bindingBytes + handlerBytes + inlineBytes + header.StringBytes;
            if (cooked.size() < total)
            {
                return std::unexpected(Corrupt(id, "ui document: cooked blob truncated"));
            }

            vector<u64> styleSheetIds(header.StyleSheetCount);
            if (styleSheetBytes > 0)
            {
                std::memcpy(styleSheetIds.data(), cooked.data() + cursor, styleSheetBytes);
            }
            cursor += styleSheetBytes;

            vector<CookedUIElement> elements(header.ElementCount);
            if (elementBytes > 0)
            {
                std::memcpy(elements.data(), cooked.data() + cursor, elementBytes);
            }
            cursor += elementBytes;

            vector<CookedUIStringSpan> classes(header.ClassCount);
            if (classBytes > 0)
            {
                std::memcpy(classes.data(), cooked.data() + cursor, classBytes);
            }
            cursor += classBytes;

            vector<CookedUIBinding> bindings(header.BindingCount);
            if (bindingBytes > 0)
            {
                std::memcpy(bindings.data(), cooked.data() + cursor, bindingBytes);
            }
            cursor += bindingBytes;

            vector<CookedUIHandler> handlers(header.HandlerCount);
            if (handlerBytes > 0)
            {
                std::memcpy(handlers.data(), cooked.data() + cursor, handlerBytes);
            }
            cursor += handlerBytes;

            vector<CookedStyleProperty> inlineProps(header.InlinePropertyCount);
            if (inlineBytes > 0)
            {
                std::memcpy(inlineProps.data(), cooked.data() + cursor, inlineBytes);
            }
            cursor += inlineBytes;

            const std::span<const u8> strings = cooked.subspan(cursor, header.StringBytes);

            // Resolves a string span into a copy, rejecting an out-of-range span.
            const auto readString = [&](const CookedUIStringSpan& span,
                                        string& out) -> AssetResult<void>
            {
                if (static_cast<usize>(span.Offset) + span.Length > header.StringBytes)
                {
                    return std::unexpected(Corrupt(id, "ui document: string span out of bounds"));
                }
                out.assign(reinterpret_cast<const char*>(strings.data()) + span.Offset,
                           span.Length);
                return {};
            };

            DecodedUIDocument decoded;
            decoded.StyleSheetIds.reserve(header.StyleSheetCount);
            for (const u64 sheetId : styleSheetIds)
            {
                decoded.StyleSheetIds.push_back(AssetId{sheetId});
            }

            decoded.Elements.reserve(header.ElementCount);
            for (const CookedUIElement& ce : elements)
            {
                if (static_cast<usize>(ce.FirstClass) + ce.ClassCount > header.ClassCount ||
                    static_cast<usize>(ce.FirstBinding) + ce.BindingCount > header.BindingCount ||
                    static_cast<usize>(ce.FirstHandler) + ce.HandlerCount > header.HandlerCount ||
                    static_cast<usize>(ce.FirstInlineProperty) + ce.InlinePropertyCount >
                        header.InlinePropertyCount)
                {
                    return std::unexpected(
                        Corrupt(id, "ui document: element side-table range out of bounds"));
                }

                Gui::UIElementRecipe recipe;
                recipe.Kind = static_cast<Gui::ElementKind>(ce.Kind);
                recipe.ChildCount = ce.ChildCount;
                recipe.IsLocKey = ce.IsLocKey != 0;
                recipe.Src = AssetId{ce.Src};
                recipe.Tint = {ce.Tint[0], ce.Tint[1], ce.Tint[2], ce.Tint[3]};
                recipe.Uv = Gui::Rect{.Min = {ce.Uv[0], ce.Uv[1]}, .Size = {ce.Uv[2], ce.Uv[3]}};
                // Component-boundary provenance and its reserved driver id ride the recipe; neither
                // is a load-time dependency — the spliced fragment elements carry their own.
                recipe.ComponentSource = AssetId{ce.ComponentSource};
                recipe.ComponentDriver = ce.ComponentDriver;
                if (ce.Src != 0)
                {
                    AddUnique(decoded.TextureIds, AssetId{ce.Src});
                }

                if (const AssetResult<void> r = readString(ce.Id, recipe.Id); !r)
                {
                    return std::unexpected(r.error());
                }
                if (const AssetResult<void> r = readString(ce.Text, recipe.Text); !r)
                {
                    return std::unexpected(r.error());
                }

                recipe.Classes.reserve(ce.ClassCount);
                for (u32 i = 0; i < ce.ClassCount; ++i)
                {
                    string tag;
                    if (const AssetResult<void> r = readString(classes[ce.FirstClass + i], tag); !r)
                    {
                        return std::unexpected(r.error());
                    }
                    recipe.Classes.push_back(std::move(tag));
                }

                recipe.Bindings.reserve(ce.BindingCount);
                for (u32 i = 0; i < ce.BindingCount; ++i)
                {
                    const CookedUIBinding& cb = bindings[ce.FirstBinding + i];
                    Gui::UIBindingRecipe binding;
                    if (const AssetResult<void> r = readString(cb.Property, binding.Property); !r)
                    {
                        return std::unexpected(r.error());
                    }
                    if (const AssetResult<void> r = readString(cb.Expression, binding.Expression);
                        !r)
                    {
                        return std::unexpected(r.error());
                    }
                    recipe.Bindings.push_back(std::move(binding));
                }

                recipe.Handlers.reserve(ce.HandlerCount);
                for (u32 i = 0; i < ce.HandlerCount; ++i)
                {
                    const CookedUIHandler& ch = handlers[ce.FirstHandler + i];
                    Gui::UIHandlerRecipe handler;
                    if (const AssetResult<void> r = readString(ch.Event, handler.Event); !r)
                    {
                        return std::unexpected(r.error());
                    }
                    if (const AssetResult<void> r = readString(ch.Handler, handler.Handler); !r)
                    {
                        return std::unexpected(r.error());
                    }
                    recipe.Handlers.push_back(std::move(handler));
                }

                recipe.InlineStyle.reserve(ce.InlinePropertyCount);
                for (u32 i = 0; i < ce.InlinePropertyCount; ++i)
                {
                    const CookedStyleProperty& cp = inlineProps[ce.FirstInlineProperty + i];
                    Gui::StyleDeclaration declaration;
                    declaration.Property = static_cast<Gui::StyleProperty>(cp.Property);
                    declaration.Unit = cp.Unit;
                    declaration.Values = {cp.Values[0], cp.Values[1], cp.Values[2], cp.Values[3]};
                    declaration.Handle = AssetId{cp.Handle};
                    if (declaration.Property == Gui::StyleProperty::TextFont)
                    {
                        AddUnique(decoded.FontIds, AssetId{cp.Handle});
                    }
                    if (declaration.Property == Gui::StyleProperty::BackgroundImage)
                    {
                        AddUnique(decoded.TextureIds, AssetId{cp.Handle});
                    }
                    if (declaration.Property == Gui::StyleProperty::BackgroundMaterial ||
                        declaration.Property == Gui::StyleProperty::ImageMaterial)
                    {
                        AddUnique(decoded.MaterialIds, AssetId{cp.Handle});
                    }
                    recipe.InlineStyle.push_back(declaration);
                }

                decoded.Elements.push_back(std::move(recipe));
            }

            return decoded;
        }
    }

    AssetResult<Detail::ParsedAsset> UIDocumentLoader::Parse(const AssetParseContext& /*context*/,
                                                             const AssetId id,
                                                             const std::span<const u8> cooked) const
    {
        AssetResult<Detail::DecodedUIDocument> decoded = Detail::DecodeUIDocument(id, cooked);
        if (!decoded)
        {
            return std::unexpected(decoded.error());
        }

        // Each referenced stylesheet is a dependency whose handle the recipe keeps, so the document
        // finalizes only once its stylesheets and their fonts are resident (the
        // material-pulls-its-textures shape). The inline styles' fonts, an Image element's source
        // texture and an inline `background-material` / `material` instance are ordinary load-time
        // dependencies too, kept resident so an instantiate-time resolve is a cache hit.
        Detail::ParsedAsset parsed;
        for (const AssetId sheetId : decoded->StyleSheetIds)
        {
            parsed.Dependencies.push_back({.Type = AssetTypes::StyleSheet, .Id = sheetId});
        }
        for (const AssetId fontId : decoded->FontIds)
        {
            parsed.Dependencies.push_back({.Type = AssetTypes::Font, .Id = fontId});
        }
        for (const AssetId textureId : decoded->TextureIds)
        {
            parsed.Dependencies.push_back({.Type = AssetTypes::Texture, .Id = textureId});
        }
        for (const AssetId materialId : decoded->MaterialIds)
        {
            parsed.Dependencies.push_back({.Type = AssetTypes::MaterialInstance, .Id = materialId});
        }

        const usize sheetCount = decoded->StyleSheetIds.size();
        auto elements = CreateRef<vector<Gui::UIElementRecipe>>(std::move(decoded->Elements));
        parsed.Complete = [elements, sheetCount](
                              AssetManager&, std::span<const Ref<Detail::AssetCacheEntry>> resolved)
            -> AssetResult<Detail::LoadJob>
        {
            vector<AssetHandle<Gui::StyleSheet>> styleSheets;
            styleSheets.reserve(sheetCount);
            for (usize i = 0; i < sheetCount; ++i)
            {
                styleSheets.push_back(AssetManager::HandleOf<Gui::StyleSheet>(resolved[i]));
            }
            vector<Ref<Detail::AssetCacheEntry>> dependencies(resolved.begin(), resolved.end());
            const Ref<Gui::UIDocument> document =
                Gui::UIDocument::Create(std::move(*elements), std::move(styleSheets), dependencies);
            return Detail::LoadJob{
                .Resource = Detail::RefAny(document),
                .Dependencies = std::move(dependencies),
                .Finalize = []() -> VoidResult { return {}; },
            };
        };
        return parsed;
    }
}
