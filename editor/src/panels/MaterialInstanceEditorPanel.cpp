#include "MaterialInstanceEditorPanel.h"

#include "AssetChip.h"
#include "AssetSourceIndex.h"
#include "EditorIcons.h"

#include <Veng/Application.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Log.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Time.h>
#include <Veng/UI/UI.h>

#include <nlohmann/json.hpp>

#include <cstring>
#include <fstream>
#include <sstream>

namespace VengEditor
{
    using namespace Veng;
    using Json = nlohmann::json;

    namespace
    {
        constexpr f32 DebounceSeconds = 0.3f;
        constexpr uvec2 PreviewExtent{256, 256};

        // Reads Components float channels from the parent default block at a field's offset.
        vec4 ReadParamDefault(std::span<const std::byte> block, const MaterialField& field)
        {
            vec4 value{0.0f, 0.0f, 0.0f, 1.0f};
            const u32 count = std::min<u32>(field.Size / sizeof(f32), 4u);
            if (static_cast<usize>(field.Offset) + count * sizeof(f32) <= block.size())
            {
                std::memcpy(&value, block.data() + field.Offset, count * sizeof(f32));
            }
            return value;
        }
    }

    MaterialInstanceEditorPanel::MaterialInstanceEditorPanel(AssetId id, path sourcePath,
                                                             const AssetSourceIndex& sources,
                                                             Application& app, AssetManager& assets,
                                                             ImGuiLayer& imgui, CookDriver cook)
        : m_Sources(sources), m_Context(app.GetRenderContext()), m_Assets(assets), m_ImGui(imgui),
          m_Cook(std::move(cook)), m_Id(id), m_SourcePath(std::move(sourcePath))
    {
        m_Title = fmt::format("Material Instance: {}", m_SourcePath.filename().string());

        // The temp cook source is a fixed dotfile beside the real source so the importer's
        // source-dir-relative parent resolution still resolves.
        m_TempPath = m_SourcePath.parent_path() /
                     fmt::format(".{}.editor-tmp.vmatinst.json", m_SourcePath.stem().string());

        m_Preview = CreateUnique<MaterialPreview>(m_Context, m_Assets, m_ImGui, PreviewExtent);
        app.RegisterViewport(m_Preview->GetViewport());

        if (!LoadInstance())
        {
            return;
        }

        TriggerCook();
    }

    MaterialInstanceEditorPanel::~MaterialInstanceEditorPanel()
    {
        m_Preview.reset();

        std::error_code ec;
        std::filesystem::remove(m_TempPath, ec);
    }

    void MaterialInstanceEditorPanel::ReadSource()
    {
        m_AuthoredParams.clear();
        m_AuthoredTextures.clear();

        const std::ifstream file(m_SourcePath, std::ios::binary);
        if (!file)
        {
            return;
        }
        std::ostringstream contents;
        contents << file.rdbuf();
        const Json doc = Json::parse(contents.str(), nullptr, false);
        if (doc.is_discarded() || !doc.is_object())
        {
            return;
        }

        if (doc.contains("parent") && doc["parent"].is_number_unsigned())
        {
            m_ParentId = AssetId{doc["parent"].get<u64>()};
        }

        if (!doc.contains("overrides") || !doc["overrides"].is_object())
        {
            return;
        }
        for (const auto& [name, value] : doc["overrides"].items())
        {
            if (value.is_number_unsigned())
            {
                // A bare unsigned value is a texture override id.
                m_AuthoredTextures[name] = AssetId{value.get<u64>()};
            }
            else if (value.is_object() && value.contains("id") && value["id"].is_number_unsigned())
            {
                m_AuthoredTextures[name] = AssetId{value["id"].get<u64>()};
            }
            else if (value.is_number())
            {
                m_AuthoredParams[name] = vec4(value.get<f32>(), 0.0f, 0.0f, 0.0f);
            }
            else if (value.is_array())
            {
                vec4 v{0.0f, 0.0f, 0.0f, 0.0f};
                for (usize i = 0; i < value.size() && i < 4; ++i)
                {
                    if (value[i].is_number())
                    {
                        v[static_cast<int>(i)] = value[i].get<f32>();
                    }
                }
                m_AuthoredParams[name] = v;
            }
        }
    }

    bool MaterialInstanceEditorPanel::LoadInstance()
    {
        ReadSource();

        // Load through the default-instance rule: the previewed asset is this instance over its
        // parent, and GetFields()/GetDomain() delegate to the parent's schema. The parent id is
        // resolved from the source so the schema is read from the authored parent even before the
        // first cook.
        const AssetId loadId = m_ParentId.IsValid() ? m_ParentId : m_Id;
        const AssetResult<AssetHandle<MaterialInstance>> loaded =
            m_Assets.LoadSync<MaterialInstance>(loadId);
        if (!loaded)
        {
            m_CookError = loaded.error().Detail;
            Log::Error("Material-instance editor: failed to load parent 0x{:X}: {}", loadId.Value,
                       loaded.error().Detail);
            return false;
        }
        m_Parent = *loaded;
        if (!m_ParentId.IsValid())
        {
            m_ParentId = loadId;
        }

        BuildSlots();
        return true;
    }

    void MaterialInstanceEditorPanel::BuildSlots()
    {
        m_Slots.clear();

        // The override surface is exactly the parent's exposed fields; a Material parent doubles
        // as a zero-override default instance whose GetParent() owns the default block to seed from.
        const std::span<const std::byte> defaultBlock =
            m_Parent.Get()->GetParent().Get()->GetDefaultBlock();

        for (const MaterialField& field : m_Parent.Get()->GetFields())
        {
            // A sampler handle mirrors a texture field, so it is not an independent override surface.
            if (field.Kind == MaterialField::FieldKind::SamplerHandle)
            {
                continue;
            }

            OverrideSlot slot;
            slot.Name = field.Name;

            if (field.Kind == MaterialField::FieldKind::TextureHandle)
            {
                slot.IsTexture = true;
                slot.Components = 0;
                const auto it = m_AuthoredTextures.find(field.Name);
                slot.Overridden = it != m_AuthoredTextures.end();
                slot.Texture = slot.Overridden ? it->second : AssetId{field.TextureId};
            }
            else
            {
                slot.IsTexture = false;
                slot.Components = std::min<u32>(field.Size / sizeof(f32), 4u);
                const auto it = m_AuthoredParams.find(field.Name);
                slot.Overridden = it != m_AuthoredParams.end();
                slot.Value = slot.Overridden ? it->second : ReadParamDefault(defaultBlock, field);
            }

            m_Slots.push_back(std::move(slot));
        }
    }

    string MaterialInstanceEditorPanel::AssembleDocument() const
    {
        Json doc = Json::object();
        doc["parent"] = m_ParentId.Value;

        Json overrides = Json::object();
        for (const OverrideSlot& slot : m_Slots)
        {
            if (!slot.Overridden)
            {
                continue;
            }
            if (slot.IsTexture)
            {
                if (slot.Texture.IsValid())
                {
                    overrides[slot.Name] = slot.Texture.Value;
                }
            }
            else if (slot.Components == 1)
            {
                overrides[slot.Name] = slot.Value.x;
            }
            else
            {
                Json arr = Json::array();
                for (u32 i = 0; i < slot.Components; ++i)
                {
                    arr.push_back(slot.Value[static_cast<int>(i)]);
                }
                overrides[slot.Name] = std::move(arr);
            }
        }
        doc["overrides"] = std::move(overrides);

        return doc.dump(2);
    }

    bool MaterialInstanceEditorPanel::WriteDocument(const path& target)
    {
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            m_CookError = fmt::format("failed to write {}", target.string());
            Log::Error("Material-instance editor: {}", *m_CookError);
            return false;
        }
        out << AssembleDocument() << '\n';
        return true;
    }

    void MaterialInstanceEditorPanel::MarkDirty()
    {
        m_CookPending = true;
        m_DebounceRemaining = DebounceSeconds;
    }

    void MaterialInstanceEditorPanel::TriggerCook()
    {
        if (m_Cooking)
        {
            return;
        }

        if (!WriteDocument(m_TempPath))
        {
            return;
        }

        m_Cooking = true;
        m_CookError.reset();

        m_Cook({.SourcePath = m_TempPath, .TargetId = m_Id, .Type = AssetType::MaterialInstance},
               [this](Result<MountHandle> mount)
               {
                   m_Cooking = false;
                   if (!mount)
                   {
                       m_CookError = mount.error();
                       Log::Error("Material-instance editor: cook failed: {}", mount.error());
                       return;
                   }

                   m_Mount = std::move(*mount);
                   m_Handle = m_Assets.Load<MaterialInstance>(m_Id);
                   m_InstanceDirty = true;
               });
    }

    void MaterialInstanceEditorPanel::OnUI()
    {
        if (m_CookPending)
        {
            m_DebounceRemaining -= Time::GetDeltaTime();
            if (m_DebounceRemaining <= 0.0f)
            {
                m_CookPending = false;
                TriggerCook();
            }
        }

        if (m_InstanceDirty && m_Handle.IsLoaded())
        {
            m_Preview->SetMaterial(m_Handle);
            m_InstanceDirty = false;
            m_PreviewReady = true;
        }

        m_Preview->Update();

        if (!m_Parent.IsLoaded())
        {
            UI::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, "Material instance failed to load");
            if (m_CookError)
            {
                UI::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, *m_CookError);
            }
            return;
        }

        // Toolbar.
        if (UI::Button(Icons::Save))
        {
            WriteDocument(m_SourcePath);
        }
        UI::Tooltip("Save the overrides to the .vmatinst.json");
        if (m_Cooking)
        {
            UI::SameLine();
            UI::Text("Cooking...");
        }
        if (m_CookError)
        {
            UI::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, fmt::format("Cook error: {}", *m_CookError));
        }

        UI::Separator();

        const f32 sideWidth = 280.0f;
        if (auto side = UI::Child("InstSide", vec2(sideWidth, 0)))
        {
            const f32 previewSide = PreviewExtent.x;
            if (m_PreviewReady)
            {
                UI::Image(m_Preview->GetTexture(), vec2(previewSide, previewSide));
            }
            else
            {
                UI::Text("Preview loading...");
            }
        }

        UI::SameLine();

        bool mutated = false;
        if (auto canvas = UI::Child("InstFields"))
        {
            // Parent picker.
            UI::Text("Parent");
            const AssetChipInfo parentChip{
                .Id = m_ParentId,
                .Type = AssetType::Material,
                .IdScope = "instparent",
                .DropTarget = true,
            };
            if (const optional<AssetId> picked = DrawAssetChip(parentChip, m_Sources))
            {
                if (picked->Value != m_ParentId.Value)
                {
                    // A new parent changes the schema entirely; reload it and rebuild the slots,
                    // dropping the prior parent's authored overrides that no longer apply.
                    m_ParentId = *picked;
                    m_AuthoredParams.clear();
                    m_AuthoredTextures.clear();
                    const AssetResult<AssetHandle<MaterialInstance>> reloaded =
                        m_Assets.LoadSync<MaterialInstance>(m_ParentId);
                    if (reloaded)
                    {
                        m_Parent = *reloaded;
                        BuildSlots();
                        mutated = true;
                    }
                }
            }

            UI::Separator();
            UI::Text("Overrides");

            // Per-field override toggle over the parent's exposed schema.
            for (OverrideSlot& slot : m_Slots)
            {
                auto id = UI::PushId(slot.Name);

                if (UI::Checkbox("##ovr", slot.Overridden))
                {
                    mutated = true;
                }
                UI::SameLine();
                UI::Text(slot.Name);

                auto disabled = UI::Disabled(!slot.Overridden);
                if (slot.IsTexture)
                {
                    const AssetChipInfo texChip{
                        .Id = slot.Texture,
                        .Type = AssetType::Texture,
                        .IdScope = "insttex",
                        .DropTarget = slot.Overridden,
                    };
                    if (const optional<AssetId> picked = DrawAssetChip(texChip, m_Sources))
                    {
                        slot.Texture = *picked;
                        mutated = true;
                    }
                }
                else if (slot.Components == 1)
                {
                    if (UI::Drag("##val", slot.Value.x))
                    {
                        mutated = true;
                    }
                }
                else if (slot.Components == 2)
                {
                    vec2 v(slot.Value);
                    if (UI::Drag("##val", v))
                    {
                        slot.Value = vec4(v, slot.Value.z, slot.Value.w);
                        mutated = true;
                    }
                }
                else if (slot.Components == 3)
                {
                    vec3 v(slot.Value);
                    if (UI::Drag("##val", v))
                    {
                        slot.Value = vec4(v, slot.Value.w);
                        mutated = true;
                    }
                }
                else
                {
                    if (UI::Drag("##val", slot.Value))
                    {
                        mutated = true;
                    }
                }
            }
        }

        if (mutated)
        {
            MarkDirty();
        }
    }
}
