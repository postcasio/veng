#include "AssetEditorPanel.h"

#include <Veng/UI/UI.h>
#include <Veng/Vendor/ImGuiInternal.h>

#include <atomic>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // Monotonic across all asset editors in a session, so two open editors of the
        // same asset get distinct window ids and dock classes.
        std::atomic<u32> s_NextInstanceId{1};
    }

    AssetEditorPanel::AssetEditorPanel()
        : m_InstanceId(s_NextInstanceId.fetch_add(1)),
          m_DockSpaceName(fmt::format("##assetdock{}", m_InstanceId))
    {
    }

    usize AssetEditorPanel::AddChild(Unique<EditorPanel> child)
    {
        const usize index = m_Children.size();
        string windowName = fmt::format("{}##doc{}", child->GetTitle(), m_InstanceId);
        m_Children.push_back({.Panel = std::move(child), .WindowName = std::move(windowName)});
        return index;
    }

    void AssetEditorPanel::DockChildWindow(usize index, u32 node)
    {
        ImGui::DockBuilderDockWindow(m_Children[index].WindowName.c_str(), node);
    }

    void AssetEditorPanel::OnRender(Renderer::CommandBuffer& cmd)
    {
        for (Child& child : m_Children)
        {
            if (child.Open)
            {
                child.Panel->OnRender(cmd);
            }
        }
    }

    void AssetEditorPanel::Draw(bool* open)
    {
        // Restrict docking to this editor's own children: the dockspace adopts this
        // class, and each child window is tagged with it, so a child cannot dock into
        // the main host dockspace and a foreign window cannot dock into this one.
        ImGuiWindowClass dockClass;
        dockClass.ClassId = m_InstanceId;
        dockClass.DockingAllowUnclassed = false;

        const ImGuiID dockspaceId = ImGui::GetID(m_DockSpaceName.c_str());

        {
            const UI::StyleVarScope padding =
                UI::StyleVar(UI::StyleVarId::WindowPadding, vec2(0, 0));
            if (auto window = UI::Window(GetTitle(), open))
            {
                // Build the split once, the first frame the node is live (or after a
                // layout reset clears it).
                if (!m_LayoutBuilt || ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
                {
                    ImGui::DockBuilderRemoveNode(dockspaceId);
                    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
                    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetContentRegionAvail());
                    BuildDefaultLayout(dockspaceId);
                    ImGui::DockBuilderFinish(dockspaceId);
                    m_LayoutBuilt = true;
                }

                OnImGui();
                ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_None, &dockClass);
            }
        }

        // Child windows are submitted at the top level (the document window's scope has
        // closed), so ImGui routes them into the dockspace by their class.
        for (Child& child : m_Children)
        {
            if (!child.Open)
            {
                continue;
            }

            ImGui::SetNextWindowClass(&dockClass);

            const UI::WindowFlags flags = child.Panel->GetWindowFlags();
            const bool noPadding = (flags & UI::WindowFlags::NoScrollbar) != UI::WindowFlags::None;
            optional<UI::StyleVarScope> padding;
            if (noPadding)
            {
                padding.emplace(UI::StyleVar(UI::StyleVarId::WindowPadding, vec2(0, 0)));
            }

            if (auto window = UI::Window(child.WindowName, &child.Open, flags))
            {
                child.Panel->OnImGui();
            }
        }
    }
}
