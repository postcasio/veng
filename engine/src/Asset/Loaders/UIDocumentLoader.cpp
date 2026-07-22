#include "UIDocumentLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Font.h>
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
                recipe.Src = AssetId{ce.Src};
                recipe.Tint = {ce.Tint[0], ce.Tint[1], ce.Tint[2], ce.Tint[3]};
                recipe.Uv = Gui::Rect{.Min = {ce.Uv[0], ce.Uv[1]}, .Size = {ce.Uv[2], ce.Uv[3]}};
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
                    recipe.InlineStyle.push_back(declaration);
                }

                decoded.Elements.push_back(std::move(recipe));
            }

            return decoded;
        }
    }

    AssetResult<Detail::LoadJob>
    UIDocumentLoader::Load(AssetManager& manager, Renderer::Context& /*context*/,
                           TaskSystem& /*tasks*/, TypeRegistry& /*types*/, AssetId id,
                           std::span<const u8> cooked, bool async) const
    {
        const AssetResult<Detail::DecodedUIDocument> decoded = Detail::DecodeUIDocument(id, cooked);
        if (!decoded)
        {
            return std::unexpected(decoded.error());
        }

        vector<Ref<Detail::AssetCacheEntry>> dependencies;
        vector<AssetHandle<Gui::StyleSheet>> styleSheets;
        styleSheets.reserve(decoded->StyleSheetIds.size());

        // Load each referenced stylesheet; keep the handle for the recipe and its entry as a
        // dependency, so the document finalizes only once its stylesheets and their fonts are
        // resident (the material-pulls-its-textures shape).
        for (const AssetId sheetId : decoded->StyleSheetIds)
        {
            if (async)
            {
                AssetHandle<Gui::StyleSheet> handle = manager.Load<Gui::StyleSheet>(sheetId);
                if (!AssetManager::EntryOf(handle))
                {
                    return std::unexpected(AssetLoadError{
                        .Kind = AssetError::MissingDependency,
                        .Id = sheetId,
                        .Detail =
                            fmt::format("ui document {}: stylesheet dependency {} did not resolve",
                                        id.Value, sheetId.Value)});
                }
                dependencies.push_back(AssetManager::EntryOf(handle));
                styleSheets.push_back(std::move(handle));
            }
            else
            {
                AssetResult<AssetHandle<Gui::StyleSheet>> handle =
                    manager.LoadSync<Gui::StyleSheet>(sheetId);
                if (!handle)
                {
                    return std::unexpected(handle.error());
                }
                dependencies.push_back(AssetManager::EntryOf(*handle));
                styleSheets.push_back(std::move(*handle));
            }
        }

        // The inline styles' fonts are ordinary load-time dependencies, kept resident.
        for (const AssetId fontId : decoded->FontIds)
        {
            if (async)
            {
                const AssetHandle<Font> handle = manager.Load<Font>(fontId);
                if (!AssetManager::EntryOf(handle))
                {
                    return std::unexpected(AssetLoadError{
                        .Kind = AssetError::MissingDependency,
                        .Id = fontId,
                        .Detail = fmt::format("ui document {}: font dependency {} did not resolve",
                                              id.Value, fontId.Value)});
                }
                dependencies.push_back(AssetManager::EntryOf(handle));
            }
            else
            {
                const AssetResult<AssetHandle<Font>> handle = manager.LoadSync<Font>(fontId);
                if (!handle)
                {
                    return std::unexpected(handle.error());
                }
                dependencies.push_back(AssetManager::EntryOf(*handle));
            }
        }

        // An Image element's source texture is an ordinary load-time dependency, kept resident so
        // the instantiate-time resolve is a cache hit and the texture stays loaded like a font.
        for (const AssetId textureId : decoded->TextureIds)
        {
            if (async)
            {
                const AssetHandle<Texture> handle = manager.Load<Texture>(textureId);
                if (!AssetManager::EntryOf(handle))
                {
                    return std::unexpected(AssetLoadError{
                        .Kind = AssetError::MissingDependency,
                        .Id = textureId,
                        .Detail =
                            fmt::format("ui document {}: texture dependency {} did not resolve",
                                        id.Value, textureId.Value)});
                }
                dependencies.push_back(AssetManager::EntryOf(handle));
            }
            else
            {
                const AssetResult<AssetHandle<Texture>> handle =
                    manager.LoadSync<Texture>(textureId);
                if (!handle)
                {
                    return std::unexpected(handle.error());
                }
                dependencies.push_back(AssetManager::EntryOf(*handle));
            }
        }

        const Ref<Gui::UIDocument> document = Gui::UIDocument::Create(
            std::move(decoded->Elements), std::move(styleSheets), dependencies);

        return Detail::LoadJob{
            .Resource = Detail::RefAny(document),
            .Dependencies = std::move(dependencies),
            .Finalize = []() -> VoidResult { return {}; },
        };
    }
}
