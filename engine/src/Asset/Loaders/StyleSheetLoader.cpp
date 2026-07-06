#include "StyleSheetLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Font.h>

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }

        // Reads a nul-terminated fixed-capacity selector name into a string, stopping at the nul.
        string ReadName(const char* field, usize capacity)
        {
            usize length = 0;
            while (length < capacity && field[length] != '\0')
            {
                ++length;
            }
            return string(field, length);
        }
    }

    namespace Detail
    {
        AssetResult<DecodedStyleSheet> DecodeStyleSheet(AssetId id, std::span<const u8> cooked)
        {
            if (cooked.size() < sizeof(CookedStyleSheetHeader))
            {
                return std::unexpected(
                    Corrupt(id, "stylesheet: cooked blob smaller than CookedStyleSheetHeader"));
            }

            CookedStyleSheetHeader header;
            std::memcpy(&header, cooked.data(), sizeof(header));

            if (header.Version != CookedStyleSheetVersion)
            {
                return std::unexpected(Corrupt(
                    id,
                    fmt::format("stylesheet: blob version {} does not match expected version {}",
                                header.Version, CookedStyleSheetVersion)));
            }

            const usize ruleBytes = static_cast<usize>(header.RuleCount) * sizeof(CookedStyleRule);
            const usize propertyBytes =
                static_cast<usize>(header.PropertyCount) * sizeof(CookedStyleProperty);

            usize cursor = sizeof(CookedStyleSheetHeader);
            if (cooked.size() < cursor + ruleBytes + propertyBytes)
            {
                return std::unexpected(Corrupt(id, "stylesheet: cooked blob truncated"));
            }

            vector<CookedStyleRule> cookedRules(header.RuleCount);
            if (ruleBytes > 0)
            {
                std::memcpy(cookedRules.data(), cooked.data() + cursor, ruleBytes);
            }
            cursor += ruleBytes;

            vector<CookedStyleProperty> cookedProperties(header.PropertyCount);
            if (propertyBytes > 0)
            {
                std::memcpy(cookedProperties.data(), cooked.data() + cursor, propertyBytes);
            }

            DecodedStyleSheet decoded;
            decoded.Rules.reserve(header.RuleCount);

            for (const CookedStyleRule& cookedRule : cookedRules)
            {
                if (static_cast<usize>(cookedRule.FirstProperty) + cookedRule.PropertyCount >
                    header.PropertyCount)
                {
                    return std::unexpected(
                        Corrupt(id, "stylesheet: rule declaration range out of bounds"));
                }

                Gui::StyleRule rule;
                rule.Type = ReadName(cookedRule.Type, StyleSelectorNameCapacity);
                rule.Class = ReadName(cookedRule.Class, StyleSelectorNameCapacity);
                rule.Id = ReadName(cookedRule.Id, StyleSelectorNameCapacity);
                rule.State = static_cast<Gui::ElementState>(cookedRule.State);
                rule.Declarations.reserve(cookedRule.PropertyCount);

                for (u32 i = 0; i < cookedRule.PropertyCount; ++i)
                {
                    const CookedStyleProperty& cp = cookedProperties[cookedRule.FirstProperty + i];
                    Gui::StyleDeclaration declaration;
                    declaration.Property = static_cast<Gui::StyleProperty>(cp.Property);
                    declaration.Unit = cp.Unit;
                    declaration.Values = {cp.Values[0], cp.Values[1], cp.Values[2], cp.Values[3]};
                    declaration.Font = AssetId{cp.Handle};
                    if (declaration.Property == Gui::StyleProperty::TextFont && cp.Handle != 0)
                    {
                        decoded.FontIds.push_back(AssetId{cp.Handle});
                    }
                    rule.Declarations.push_back(declaration);
                }

                decoded.Rules.push_back(std::move(rule));
            }

            // Deduplicate the surfaced font ids so a font referenced by many rules loads once.
            vector<AssetId> unique;
            for (const AssetId fontId : decoded.FontIds)
            {
                bool known = false;
                for (const AssetId existing : unique)
                {
                    if (existing.Value == fontId.Value)
                    {
                        known = true;
                        break;
                    }
                }
                if (!known)
                {
                    unique.push_back(fontId);
                }
            }
            decoded.FontIds = std::move(unique);

            return decoded;
        }
    }

    AssetResult<Detail::LoadJob>
    StyleSheetLoader::Load(AssetManager& manager, Renderer::Context& /*context*/,
                           TaskSystem& /*tasks*/, TypeRegistry& /*types*/, AssetId id,
                           std::span<const u8> cooked, bool async) const
    {
        const AssetResult<Detail::DecodedStyleSheet> decoded = Detail::DecodeStyleSheet(id, cooked);
        if (!decoded)
        {
            return std::unexpected(decoded.error());
        }

        vector<Ref<Detail::AssetCacheEntry>> dependencies;
        dependencies.reserve(decoded->FontIds.size());
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
                        .Detail = fmt::format("stylesheet {}: font dependency {} did not resolve",
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

        const Ref<Gui::StyleSheet> sheet =
            Gui::StyleSheet::Create(std::move(decoded->Rules), dependencies);

        return Detail::LoadJob{
            .Resource = Detail::RefAny(sheet),
            .Dependencies = std::move(dependencies),
            .Finalize = []() -> VoidResult { return {}; },
        };
    }
}
