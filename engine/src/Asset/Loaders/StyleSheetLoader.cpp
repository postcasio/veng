#include "StyleSheetLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Font.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Texture.h>

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
            const usize animationBytes =
                static_cast<usize>(header.AnimationCount) * sizeof(CookedStyleAnimation);
            const usize keyframeBytes =
                static_cast<usize>(header.KeyframeCount) * sizeof(CookedStyleKeyframe);
            const usize gradientBytes =
                static_cast<usize>(header.GradientCount) * sizeof(CookedStyleGradient);
            const usize variableBytes =
                static_cast<usize>(header.VariableCount) * sizeof(CookedStyleVariable);
            const auto rampBytes = static_cast<usize>(header.RampByteCount);

            usize cursor = sizeof(CookedStyleSheetHeader);
            if (cooked.size() < cursor + ruleBytes + propertyBytes + animationBytes +
                                    keyframeBytes + gradientBytes + variableBytes + rampBytes)
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
            cursor += propertyBytes;

            vector<CookedStyleAnimation> cookedAnimations(header.AnimationCount);
            if (animationBytes > 0)
            {
                std::memcpy(cookedAnimations.data(), cooked.data() + cursor, animationBytes);
            }
            cursor += animationBytes;

            vector<CookedStyleKeyframe> cookedKeyframes(header.KeyframeCount);
            if (keyframeBytes > 0)
            {
                std::memcpy(cookedKeyframes.data(), cooked.data() + cursor, keyframeBytes);
            }
            cursor += keyframeBytes;

            vector<CookedStyleGradient> cookedGradients(header.GradientCount);
            if (gradientBytes > 0)
            {
                std::memcpy(cookedGradients.data(), cooked.data() + cursor, gradientBytes);
            }
            cursor += gradientBytes;

            vector<CookedStyleVariable> cookedVariables(header.VariableCount);
            if (variableBytes > 0)
            {
                std::memcpy(cookedVariables.data(), cooked.data() + cursor, variableBytes);
            }
            cursor += variableBytes;

            const u8* const rampRegion = cooked.data() + cursor;

            DecodedStyleSheet decoded;
            decoded.Rules.reserve(header.RuleCount);

            // The shared property-slice decode both a rule and a keyframe read through.
            const auto readDeclarations =
                [&](u32 first, u32 count, vector<Gui::StyleDeclaration>& out) -> optional<string>
            {
                if (static_cast<usize>(first) + count > header.PropertyCount)
                {
                    return string{"declaration range out of bounds"};
                }
                out.reserve(count);
                for (u32 i = 0; i < count; ++i)
                {
                    const CookedStyleProperty& cp = cookedProperties[first + i];
                    Gui::StyleDeclaration declaration;
                    declaration.Property = static_cast<Gui::StyleProperty>(cp.Property);
                    declaration.Unit = cp.Unit;
                    declaration.Values = {cp.Values[0], cp.Values[1], cp.Values[2], cp.Values[3]};
                    declaration.Handle = AssetId{cp.Handle};
                    if (declaration.Property == Gui::StyleProperty::TextFont && cp.Handle != 0)
                    {
                        decoded.FontIds.push_back(AssetId{cp.Handle});
                    }
                    if (declaration.Property == Gui::StyleProperty::BackgroundImage &&
                        cp.Handle != 0)
                    {
                        decoded.TextureIds.push_back(AssetId{cp.Handle});
                    }
                    if ((declaration.Property == Gui::StyleProperty::BackgroundMaterial ||
                         declaration.Property == Gui::StyleProperty::ImageMaterial) &&
                        cp.Handle != 0)
                    {
                        decoded.MaterialIds.push_back(AssetId{cp.Handle});
                    }
                    out.push_back(declaration);
                }
                return std::nullopt;
            };

            for (const CookedStyleRule& cookedRule : cookedRules)
            {
                Gui::StyleRule rule;
                rule.Type = ReadName(cookedRule.Type, StyleSelectorNameCapacity);
                rule.Class = ReadName(cookedRule.Class, StyleSelectorNameCapacity);
                rule.Id = ReadName(cookedRule.Id, StyleSelectorNameCapacity);
                rule.State = static_cast<Gui::ElementState>(cookedRule.State);
                if (readDeclarations(cookedRule.FirstProperty, cookedRule.PropertyCount,
                                     rule.Declarations)
                        .has_value())
                {
                    return std::unexpected(
                        Corrupt(id, "stylesheet: rule declaration range out of bounds"));
                }
                decoded.Rules.push_back(std::move(rule));
            }

            decoded.Animations.reserve(header.AnimationCount);
            for (const CookedStyleAnimation& cookedAnimation : cookedAnimations)
            {
                if (static_cast<usize>(cookedAnimation.FirstKeyframe) +
                        cookedAnimation.KeyframeCount >
                    header.KeyframeCount)
                {
                    return std::unexpected(
                        Corrupt(id, "stylesheet: animation keyframe range out of bounds"));
                }
                Gui::StyleAnimationClip clip;
                clip.Keyframes.reserve(cookedAnimation.KeyframeCount);
                for (u32 k = 0; k < cookedAnimation.KeyframeCount; ++k)
                {
                    const CookedStyleKeyframe& cookedKey =
                        cookedKeyframes[cookedAnimation.FirstKeyframe + k];
                    Gui::StyleKeyframe key;
                    key.Offset = cookedKey.Offset;
                    if (readDeclarations(cookedKey.FirstProperty, cookedKey.PropertyCount,
                                         key.Declarations)
                            .has_value())
                    {
                        return std::unexpected(
                            Corrupt(id, "stylesheet: keyframe declaration range out of bounds"));
                    }
                    clip.Keyframes.push_back(std::move(key));
                }
                decoded.Animations.push_back(std::move(clip));
            }

            decoded.Gradients.reserve(header.GradientCount);
            for (const CookedStyleGradient& cookedGradient : cookedGradients)
            {
                const usize length =
                    static_cast<usize>(cookedGradient.RampTexels) * 4 * sizeof(u16);
                if (static_cast<usize>(cookedGradient.RampOffset) + length > rampBytes)
                {
                    return std::unexpected(
                        Corrupt(id, "stylesheet: gradient ramp range out of bounds"));
                }
                Gui::StyleGradient gradient;
                gradient.Kind = static_cast<Gui::GradientKind>(cookedGradient.Kind);
                gradient.P0 = {cookedGradient.P0[0], cookedGradient.P0[1]};
                gradient.P1 = {cookedGradient.P1[0], cookedGradient.P1[1]};
                gradient.AngleOffset = cookedGradient.AngleOffset;
                gradient.Width = cookedGradient.RampTexels;
                gradient.Ramp.assign(rampRegion + cookedGradient.RampOffset,
                                     rampRegion + cookedGradient.RampOffset + length);
                decoded.Gradients.push_back(std::move(gradient));
            }

            decoded.Variables.reserve(header.VariableCount);
            for (const CookedStyleVariable& cookedVariable : cookedVariables)
            {
                Gui::StyleVariable variable;
                variable.Name = ReadName(cookedVariable.Name, StyleSelectorNameCapacity);
                variable.Kind = static_cast<Gui::StyleVariableKind>(cookedVariable.Kind);
                variable.Payload = {cookedVariable.Payload[0], cookedVariable.Payload[1],
                                    cookedVariable.Payload[2], cookedVariable.Payload[3]};
                decoded.Variables.push_back(std::move(variable));
            }

            // Deduplicate the surfaced asset ids so an asset referenced by many rules loads once.
            const auto deduplicate = [](vector<AssetId>& ids)
            {
                vector<AssetId> unique;
                for (const AssetId id : ids)
                {
                    bool known = false;
                    for (const AssetId existing : unique)
                    {
                        if (existing.Value == id.Value)
                        {
                            known = true;
                            break;
                        }
                    }
                    if (!known)
                    {
                        unique.push_back(id);
                    }
                }
                ids = std::move(unique);
            };
            deduplicate(decoded.FontIds);
            deduplicate(decoded.TextureIds);
            deduplicate(decoded.MaterialIds);

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

        // A `background-image`'s texture is an ordinary load-time dependency, kept resident so the
        // instantiate-time resolve is a cache hit and the texture stays loaded like a font.
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
                            fmt::format("stylesheet {}: texture dependency {} did not resolve",
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

        // A `background-material` / `material` instance is an ordinary load-time dependency on the
        // same footing as a texture; the id may name a bare Material, which the instance loader
        // resolves to its zero-override default instance.
        for (const AssetId materialId : decoded->MaterialIds)
        {
            if (async)
            {
                const AssetHandle<MaterialInstance> handle =
                    manager.Load<MaterialInstance>(materialId);
                if (!AssetManager::EntryOf(handle))
                {
                    return std::unexpected(AssetLoadError{
                        .Kind = AssetError::MissingDependency,
                        .Id = materialId,
                        .Detail =
                            fmt::format("stylesheet {}: material dependency {} did not resolve",
                                        id.Value, materialId.Value)});
                }
                dependencies.push_back(AssetManager::EntryOf(handle));
            }
            else
            {
                const AssetResult<AssetHandle<MaterialInstance>> handle =
                    manager.LoadSync<MaterialInstance>(materialId);
                if (!handle)
                {
                    return std::unexpected(handle.error());
                }
                dependencies.push_back(AssetManager::EntryOf(*handle));
            }
        }

        const Ref<Gui::StyleSheet> sheet = Gui::StyleSheet::Create(
            std::move(decoded->Rules), std::move(decoded->Animations),
            std::move(decoded->Gradients), std::move(decoded->Variables), dependencies);

        return Detail::LoadJob{
            .Resource = Detail::RefAny(sheet),
            .Dependencies = std::move(dependencies),
            .Finalize = []() -> VoidResult { return {}; },
        };
    }
}
