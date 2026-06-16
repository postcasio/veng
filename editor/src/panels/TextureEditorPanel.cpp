#include "TextureEditorPanel.h"

#include <Veng/Asset/Texture.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Log.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Vendor/ImGui.h>

#include <fstream>
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
        const char* const WrapNames[] = {"repeat", "mirrored_repeat", "clamp_to_edge", "clamp_to_border"};

        template <typename E>
        optional<E> ParseEnum(const std::string& value, const char* const* names, usize count)
        {
            for (usize i = 0; i < count; ++i)
                if (value == names[i])
                    return static_cast<E>(i);
            return std::nullopt;
        }
    }

    TextureEditorPanel::TextureEditorPanel(AssetId id, path sourcePath, Renderer::Context& context,
                                           AssetManager& assets, ImGuiLayer& imgui, CookDriver cook) :
        m_Id(id), m_SourcePath(std::move(sourcePath)), m_Context(context), m_Assets(assets),
        m_ImGui(imgui), m_Cook(std::move(cook))
    {
        m_Title = fmt::format("Texture: {}", m_SourcePath.filename().string());

        m_Sampler = Renderer::Sampler::Create(m_Context, {
            .Name = "Texture Editor Preview Sampler",
            .AddressModeU = Renderer::AddressMode::ClampToEdge,
            .AddressModeV = Renderer::AddressMode::ClampToEdge,
            .AddressModeW = Renderer::AddressMode::ClampToEdge,
        });

        LoadSettings();
        TriggerCook();
    }

    TextureEditorPanel::~TextureEditorPanel()
    {
        if (m_Preview)
        {
            m_ImGui.DestroyTexture(*m_Preview);
            m_Preview.reset();
        }
    }

    void TextureEditorPanel::LoadSettings()
    {
        m_Settings = Settings{};

        std::ifstream file(m_SourcePath, std::ios::binary);
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
            m_Settings.Srgb = tex["srgb"].get<bool>();

        if (tex.contains("sampler") && tex["sampler"].is_object())
        {
            const nlohmann::json& sampler = tex["sampler"];
            auto readFilter = [&](const char* key, Filter& out)
            {
                if (sampler.contains(key) && sampler[key].is_string())
                    if (auto parsed = ParseEnum<Filter>(sampler[key].get<std::string>(), FilterNames, 2))
                        out = *parsed;
            };
            auto readWrap = [&](const char* key, Wrap& out)
            {
                if (sampler.contains(key) && sampler[key].is_string())
                    if (auto parsed = ParseEnum<Wrap>(sampler[key].get<std::string>(), WrapNames, 4))
                        out = *parsed;
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
            std::ifstream file(m_SourcePath, std::ios::binary);
            if (file)
            {
                std::ostringstream contents;
                contents << file.rdbuf();
                const nlohmann::json parsed = nlohmann::json::parse(contents.str(), nullptr, false);
                if (!parsed.is_discarded() && parsed.is_object())
                    tex = parsed;
            }
        }

        tex["srgb"] = m_Settings.Srgb;
        nlohmann::json& sampler = tex["sampler"];
        if (!sampler.is_object())
            sampler = nlohmann::json::object();
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
            return;

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

            // Replace the mount (the old archive's bytes free with the old handle's
            // drop) and re-fetch the texture; the new memory mount shadows the prior
            // cooked blob for this id. The async load lands resident on a later
            // frame's PumpFinalizes, where OnImGui rebuilds the preview.
            m_Mount = std::move(*mount);
            m_Handle = m_Assets.Load<Texture>(m_Id);
            m_PreviewDirty = true;
        });
    }

    void TextureEditorPanel::OnImGui()
    {
        // Advance the debounce: a settled change fires one cook, so a slider drag
        // does not issue a cook per frame.
        if (m_CookPending)
        {
            m_DebounceRemaining -= ImGui::GetIO().DeltaTime;
            if (m_DebounceRemaining <= 0.0f)
            {
                m_CookPending = false;
                TriggerCook();
            }
        }

        // Rebuild the preview once the freshly loaded handle is resident: drop the
        // old ImGuiTexture and create one from the new view.
        if (m_PreviewDirty && m_Handle.IsLoaded())
        {
            if (m_Preview)
                m_ImGui.DestroyTexture(*m_Preview);
            m_Preview = m_ImGui.CreateTexture(*m_Sampler, *m_Handle->GetView());
            m_PreviewDirty = false;
        }

        const ImVec2 available = ImGui::GetContentRegionAvail();
        const f32 previewSide = available.x > 16.0f ? available.x : 16.0f;
        if (m_Preview && m_Handle.IsLoaded())
        {
            ImGui::Image(static_cast<ImTextureID>(m_Preview->GetTextureId()),
                         {previewSide, previewSide});
        }
        else
        {
            ImGui::TextUnformatted(m_Cooking ? "Cooking..." : "Loading...");
        }

        if (m_Cooking)
            ImGui::TextUnformatted("Cooking...");

        if (m_CookError)
            ImGui::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, "Cook error: %s", m_CookError->c_str());

        ImGui::Separator();

        bool changed = false;
        changed |= ImGui::Checkbox("sRGB", &m_Settings.Srgb);

        auto filterCombo = [&](const char* label, Filter& value)
        {
            int current = static_cast<int>(value);
            if (ImGui::Combo(label, &current, "Nearest\0Linear\0"))
            {
                value = static_cast<Filter>(current);
                changed = true;
            }
        };
        auto wrapCombo = [&](const char* label, Wrap& value)
        {
            int current = static_cast<int>(value);
            if (ImGui::Combo(label, &current, "Repeat\0Mirrored Repeat\0Clamp To Edge\0Clamp To Border\0"))
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

        ImGui::Separator();

        if (ImGui::Button("Save"))
            SaveSettings();
        ImGui::SameLine();
        if (ImGui::Button("Revert"))
        {
            LoadSettings();
            SaveSettings();
            TriggerCook();
        }
    }
}
