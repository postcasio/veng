#include "AssetEditorPanel.h"

#include <Veng/Log.h>
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
          m_DockSpaceName(fmt::format("##assetdock{}", m_InstanceId)),
          m_ClosePromptName(fmt::format("Unsaved changes##close{}", m_InstanceId))
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

    void AssetEditorPanel::DrawSingleWindow(bool* open)
    {
        m_Focused = false;

        const UI::WindowFlags flags =
            HasUnsavedChanges() ? UI::WindowFlags::UnsavedDocument : UI::WindowFlags::None;
        if (auto window = UI::Window(GetTitle(), open, flags))
        {
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            {
                m_Focused = true;
            }
            OnUI();
        }
    }

    void AssetEditorPanel::ResolvePendingClose(bool* open)
    {
        if (!m_ClosePending)
        {
            return;
        }

        if (auto popup = UI::Popup(m_ClosePromptName))
        {
            UI::Text(fmt::format("Save changes to {} before closing?", GetTitle()));
            UI::Separator();

            if (UI::Button("Save"))
            {
                const VoidResult saved = Save();
                if (saved)
                {
                    m_ClosePending = false;
                    *open = false;
                }
                else
                {
                    // A failed save must not discard the edits it could not write; the document
                    // stays open with the error reported through the panel's own surface.
                    m_ClosePending = false;
                    Log::Error("editor: save failed: {}", saved.error());
                }
                UI::CloseCurrentPopup();
            }
            UI::SameLine();
            if (UI::Button("Discard"))
            {
                m_ClosePending = false;
                *open = false;
                UI::CloseCurrentPopup();
            }
            UI::SameLine();
            if (UI::Button("Cancel"))
            {
                m_ClosePending = false;
                UI::CloseCurrentPopup();
            }
            return;
        }

        // Dismissing the popup by clicking away is a cancel: the document stays open and dirty.
        m_ClosePending = false;
    }

    void AssetEditorPanel::Draw(bool* open)
    {
        // The host clears *open when the window's ✕ is clicked and destroys the panel after the
        // frame, so a dirty document must take the close back and ask first.
        const bool wasOpen = open != nullptr && *open;

        if (m_Children.empty())
        {
            DrawSingleWindow(open);
        }
        else
        {
            DrawDockedWindow(open);
        }

        if (wasOpen && !*open && HasUnsavedChanges())
        {
            *open = true;
            m_ClosePending = true;
            UI::OpenPopup(m_ClosePromptName);
        }
        ResolvePendingClose(open);
    }

    void AssetEditorPanel::DrawDockedWindow(bool* open)
    {
        // Restrict docking to this editor's own children: the dockspace adopts this
        // class, and each child window is tagged with it, so a child cannot dock into
        // the main host dockspace and a foreign window cannot dock into this one.
        ImGuiWindowClass dockClass;
        dockClass.ClassId = m_InstanceId;
        dockClass.DockingAllowUnclassed = false;

        const ImGuiID dockspaceId = ImGui::GetID(m_DockSpaceName.c_str());

        // Re-evaluate focus from this frame's windows: the document window or any docked child.
        m_Focused = false;

        bool documentVisible = false;
        {
            const UI::StyleVarScope padding =
                UI::StyleVar(UI::StyleVarId::WindowPadding, vec2(0, 0));
            // The unsaved marker is a window flag (an ImGui-drawn dot), not a title prefix, so a
            // clean→dirty transition never changes the window id and never steals focus from a
            // field being edited in a docked child.
            const UI::WindowFlags docFlags =
                HasUnsavedChanges() ? UI::WindowFlags::UnsavedDocument : UI::WindowFlags::None;
            if (auto window = UI::Window(GetTitle(), open, docFlags))
            {
                documentVisible = true;
                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                {
                    m_Focused = true;
                }
                // Build the default split only when no layout exists yet: a fresh node
                // (first run) or one DockSpace auto-created empty. A layout restored from
                // imgui.ini is non-empty and is left untouched, so the user's docking
                // survives a restart. The split sizes bake from the content region, which is
                // not yet the docked size on this window's appearing frame (a document the host
                // force-docks into its central node only inherits that node's size the following
                // frame); building then bakes tiny child sizes the central node never gives back.
                // Skip the appearing frame so the region is the real docked size.
                const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspaceId);
                const vec2 avail = ImGui::GetContentRegionAvail();
                if ((node == nullptr || node->IsEmpty()) && avail.x > 0.0f && avail.y > 0.0f &&
                    !ImGui::IsWindowAppearing())
                {
                    ImGui::DockBuilderRemoveNode(dockspaceId);
                    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
                    ImGui::DockBuilderSetNodeSize(dockspaceId, avail);
                    BuildDefaultLayout(dockspaceId);
                    ImGui::DockBuilderFinish(dockspaceId);
                }

                OnUI();
                ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_None, &dockClass);
            }
        }

        // When this editor is an unselected tab, the document body is skipped and the DockSpace
        // above never runs. Keep the node alive (legal from any scope under KeepAliveOnly) so its
        // docked children retain their layout, and submit no children — they hide with the document
        // rather than floating free of their now-hidden host, and re-dock when the tab is reselected.
        if (!documentVisible)
        {
            ImGui::DockSpace(dockspaceId, ImVec2(0, 0), ImGuiDockNodeFlags_KeepAliveOnly,
                             &dockClass);
            return;
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
                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                {
                    m_Focused = true;
                }
                child.Panel->OnUI();
            }
        }
    }
}
