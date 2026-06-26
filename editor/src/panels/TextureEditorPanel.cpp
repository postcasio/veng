#include "TextureEditorPanel.h"

#include <Veng/Asset/Texture.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Log.h>
#include <Veng/Project/BuildConfiguration.h>
#include <Veng/Project/CompressionFormat.h>
#include <Veng/Project/CompressionRole.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Time.h>
#include <Veng/UI/UI.h>

#include <array>
#include <fstream>
#include <span>
#include <sstream>

#include <nlohmann/json.hpp>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        constexpr f32 DebounceSeconds = 0.3f;

        // The importer's string vocabulary (TextureImporter.cpp). Kept in lockstep
        // with the enum ordinals declared in the header.
        const char* const FilterNames[] = {"nearest", "linear"};
        const char* const WrapNames[] = {"repeat", "mirrored_repeat", "clamp_to_edge",
                                         "clamp_to_border"};

        template <typename E>
        optional<E> ParseEnum(const std::string& value, const char* const* names, usize count)
        {
            for (usize i = 0; i < count; ++i)
            {
                if (value == names[i])
                {
                    return static_cast<E>(i);
                }
            }
            return std::nullopt;
        }

        // Reads the format a role resolves to from a configuration's fixed RoleToFormat record.
        CompressionFormat RoleFormat(const RoleToFormat& table, CompressionRole role)
        {
            switch (role)
            {
            case CompressionRole::Color:
                return table.Color;
            case CompressionRole::Normal:
                return table.Normal;
            case CompressionRole::Mask:
                return table.Mask;
            case CompressionRole::HDR:
                return table.HDR;
            case CompressionRole::UI:
                return table.UI;
            }
            return table.Color;
        }
    }

    TextureEditorPanel::TextureEditorPanel(AssetId id, path sourcePath, Renderer::Context& context,
                                           AssetManager& assets, ImGuiLayer& imgui, CookDriver cook,
                                           ActiveConfigAccessor activeConfig)
        : m_Id(id), m_SourcePath(std::move(sourcePath)), m_Context(context), m_Assets(assets),
          m_ImGui(imgui), m_Cook(std::move(cook)), m_ActiveConfig(std::move(activeConfig))
    {
        m_Title = fmt::format("Texture: {}", m_SourcePath.filename().string());

        m_Sampler = Renderer::Sampler::Create(
            m_Context, {
                           .Name = "Texture Editor Preview Sampler",
                           .AddressModeU = Renderer::AddressMode::ClampToEdge,
                           .AddressModeV = Renderer::AddressMode::ClampToEdge,
                           .AddressModeW = Renderer::AddressMode::ClampToEdge,
                       });

        LoadSettings();
        TriggerCook();
    }

    TextureEditorPanel::~TextureEditorPanel() = default;

    void TextureEditorPanel::LoadSettings()
    {
        m_Settings = Settings{};

        const std::ifstream file(m_SourcePath, std::ios::binary);
        if (!file)
        {
            Log::Error("Texture editor: failed to open {}", m_SourcePath.string());
            return;
        }

        std::ostringstream contents;
        contents << file.rdbuf();
        const nlohmann::json tex = nlohmann::json::parse(contents.str(), nullptr, false);
        if (tex.is_discarded() || !tex.is_object())
        {
            Log::Error("Texture editor: malformed JSON {}", m_SourcePath.string());
            return;
        }

        if (tex.contains("srgb") && tex["srgb"].is_boolean())
        {
            m_Settings.Srgb = tex["srgb"].get<bool>();
        }

        // An absent or unparseable role leaves m_Settings.Role unset (the sRGB guess); an
        // explicit role overrides it.
        if (tex.contains("role") && tex["role"].is_string())
        {
            m_Settings.Role = ParseCompressionRole(tex["role"].get<std::string>());
        }

        if (tex.contains("sampler") && tex["sampler"].is_object())
        {
            const nlohmann::json& sampler = tex["sampler"];
            auto readFilter = [&](const char* key, Filter& out)
            {
                if (sampler.contains(key) && sampler[key].is_string())
                {
                    if (auto parsed =
                            ParseEnum<Filter>(sampler[key].get<std::string>(), FilterNames, 2))
                    {
                        out = *parsed;
                    }
                }
            };
            auto readWrap = [&](const char* key, Wrap& out)
            {
                if (sampler.contains(key) && sampler[key].is_string())
                {
                    if (auto parsed =
                            ParseEnum<Wrap>(sampler[key].get<std::string>(), WrapNames, 4))
                    {
                        out = *parsed;
                    }
                }
            };

            readFilter("min", m_Settings.Min);
            readFilter("mag", m_Settings.Mag);
            readFilter("mipmap", m_Settings.Mipmap);
            readWrap("wrap_u", m_Settings.WrapU);
            readWrap("wrap_v", m_Settings.WrapV);
        }
    }

    bool TextureEditorPanel::SaveSettings()
    {
        // Read the existing file so unknown keys (image, anisotropy, hand-authored
        // structure) survive the round-trip — only the edited keys are patched.
        nlohmann::json tex = nlohmann::json::object();
        {
            const std::ifstream file(m_SourcePath, std::ios::binary);
            if (file)
            {
                std::ostringstream contents;
                contents << file.rdbuf();
                const nlohmann::json parsed = nlohmann::json::parse(contents.str(), nullptr, false);
                if (!parsed.is_discarded() && parsed.is_object())
                {
                    tex = parsed;
                }
            }
        }

        tex["srgb"] = m_Settings.Srgb;

        // An authored role writes the "role" key; clearing it removes the key so the cook reverts
        // to the sRGB guess. The raw "compression" escape-hatch key, if present, is left untouched.
        if (m_Settings.Role)
        {
            tex["role"] = std::string(ToString(*m_Settings.Role));
        }
        else
        {
            tex.erase("role");
        }

        nlohmann::json& sampler = tex["sampler"];
        if (!sampler.is_object())
        {
            sampler = nlohmann::json::object();
        }
        sampler["min"] = FilterNames[static_cast<u32>(m_Settings.Min)];
        sampler["mag"] = FilterNames[static_cast<u32>(m_Settings.Mag)];
        sampler["mipmap"] = FilterNames[static_cast<u32>(m_Settings.Mipmap)];
        sampler["wrap_u"] = WrapNames[static_cast<u32>(m_Settings.WrapU)];
        sampler["wrap_v"] = WrapNames[static_cast<u32>(m_Settings.WrapV)];

        std::ofstream out(m_SourcePath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            m_CookError = fmt::format("failed to write {}", m_SourcePath.string());
            Log::Error("Texture editor: {}", *m_CookError);
            return false;
        }
        out << tex.dump(4) << '\n';
        return true;
    }

    void TextureEditorPanel::TriggerCook()
    {
        if (m_Cooking)
        {
            return;
        }

        m_Cooking = true;
        m_CookError.reset();

        m_Cook({.SourcePath = m_SourcePath, .TargetId = m_Id, .Type = AssetType::Texture},
               [this](Result<MountHandle> mount)
               {
                   m_Cooking = false;
                   if (!mount)
                   {
                       m_CookError = mount.error();
                       return;
                   }

                   // Replace the mount and re-fetch; OnUI rebuilds the preview once
                   // the async load lands resident.
                   m_Mount = std::move(*mount);
                   m_Handle = m_Assets.Load<Texture>(m_Id);
                   m_PreviewDirty = true;
               });
    }

    void TextureEditorPanel::OnUI()
    {
        // Debounce so a slider drag does not fire a cook per frame.
        if (m_CookPending)
        {
            m_DebounceRemaining -= Time::GetDeltaTime();
            if (m_DebounceRemaining <= 0.0f)
            {
                m_CookPending = false;
                TriggerCook();
            }
        }

        // Rebuild the preview once the freshly cooked texture is resident.
        if (m_PreviewDirty && m_Handle.IsLoaded())
        {
            m_Preview = m_ImGui.CreateTexture(*m_Sampler, *m_Handle->GetView());
            m_PreviewDirty = false;
        }

        const vec2 available = UI::ContentRegionAvail();
        const f32 previewSide = available.x > 16.0f ? available.x : 16.0f;
        if (m_Preview && m_Handle.IsLoaded())
        {
            UI::Image(m_Preview, {previewSide, previewSide});
        }
        else
        {
            UI::Text(m_Cooking ? "Cooking..." : "Loading...");
        }

        if (m_Cooking)
        {
            UI::Text("Cooking...");
        }

        if (m_CookError)
        {
            UI::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, fmt::format("Cook error: {}", *m_CookError));
        }

        UI::Separator();

        static constexpr std::array<string_view, 2> FilterItems{"Nearest", "Linear"};
        static constexpr std::array<string_view, 4> WrapItems{"Repeat", "Mirrored Repeat",
                                                              "Clamp To Edge", "Clamp To Border"};

        bool changed = false;
        changed |= UI::Checkbox("sRGB", m_Settings.Srgb);

        // The compression role: index 0 clears it (the sRGB guess), index N+1 authors role N.
        // The texture declares a role (its intent); a build configuration owns the codec, so the
        // raw "compression" escape hatch is not authored here.
        {
            std::array<string_view, CompressionRoles.size() + 1> roleItems;
            roleItems[0] = "(guess from sRGB)";
            for (usize i = 0; i < CompressionRoles.size(); ++i)
            {
                roleItems[i + 1] = ToString(CompressionRoles[i]);
            }

            i32 roleIndex = 0;
            if (m_Settings.Role)
            {
                roleIndex = static_cast<i32>(static_cast<u8>(*m_Settings.Role)) + 1;
            }
            if (UI::Combo("Role", roleIndex, std::span<const string_view>(roleItems)))
            {
                m_Settings.Role = roleIndex == 0
                                      ? optional<CompressionRole>{}
                                      : CompressionRoles[static_cast<usize>(roleIndex) - 1];
                changed = true;
            }
        }

        // The resolved-format read-out: the format the texture *resolves to* under the active
        // configuration, computed from the effective role (authored or the sRGB guess) and the
        // configuration's role table. Recomputed each frame so it tracks a configuration change.
        {
            const CompressionRole effectiveRole =
                m_Settings.Role
                    ? *m_Settings.Role
                    : (m_Settings.Srgb ? CompressionRole::Color : CompressionRole::Mask);
            const BuildConfiguration* config = m_ActiveConfig ? m_ActiveConfig() : nullptr;
            if (config != nullptr)
            {
                const CompressionFormat format = RoleFormat(config->Formats, effectiveRole);
                UI::TextDisabled(
                    fmt::format("→ {} for active config '{}'", ToString(format), config->Name));
            }
            else
            {
                UI::TextDisabled("→ no active build configuration (zero-config ASTC default)");
            }
        }

        auto filterCombo = [&](string_view label, Filter& value)
        {
            i32 current = static_cast<i32>(value);
            if (UI::Combo(label, current, FilterItems))
            {
                value = static_cast<Filter>(current);
                changed = true;
            }
        };
        auto wrapCombo = [&](string_view label, Wrap& value)
        {
            i32 current = static_cast<i32>(value);
            if (UI::Combo(label, current, WrapItems))
            {
                value = static_cast<Wrap>(current);
                changed = true;
            }
        };

        filterCombo("Min filter", m_Settings.Min);
        filterCombo("Mag filter", m_Settings.Mag);
        wrapCombo("Wrap U", m_Settings.WrapU);
        wrapCombo("Wrap V", m_Settings.WrapV);
        filterCombo("Mip filter", m_Settings.Mipmap);

        if (changed)
        {
            // A live recook reads the on-disk source, so persist the edit before
            // arming the debounce; the cook then picks up the change.
            SaveSettings();
            m_CookPending = true;
            m_DebounceRemaining = DebounceSeconds;
        }

        UI::Separator();

        if (UI::Button("Save"))
        {
            SaveSettings();
        }
        UI::SameLine();
        if (UI::Button("Revert"))
        {
            LoadSettings();
            SaveSettings();
            TriggerCook();
        }
    }
}
