// Drives Veng::UI's two dialog shapes against a headless ImGui context, with no platform or
// renderer backend, to pin down the one property that separates them: a Popup is dismissed by a
// click on void, and a Modal is not. The editor's unsaved-changes prompt rides on that difference.

#include <doctest/doctest.h>

#include <Veng/UI/Scopes.h>

#include <imgui.h>

using namespace Veng;

namespace
{
    // The null-backend posture from imgui's own headless example: no platform, no renderer, and
    // the renderer-has-textures flag so the atlas is never uploaded anywhere.
    class HeadlessImGui
    {
    public:
        HeadlessImGui()
        {
            IMGUI_CHECKVERSION();
            m_Context = ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1920, 1080);
            io.DeltaTime = 1.0f / 60.0f;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        }

        ~HeadlessImGui() { ImGui::DestroyContext(m_Context); }

        HeadlessImGui(const HeadlessImGui&) = delete;
        HeadlessImGui& operator=(const HeadlessImGui&) = delete;
        HeadlessImGui(HeadlessImGui&&) = delete;
        HeadlessImGui& operator=(HeadlessImGui&&) = delete;

        // Runs one frame, calling `body` between NewFrame and Render, then satisfies the texture
        // requests the frame produced so the next frame does not re-request them.
        template <typename Body>
        void Frame(Body&& body)
        {
            ImGui::NewFrame();
            body();
            ImGui::Render();
            const ImDrawData* data = ImGui::GetDrawData();
            if (data != nullptr && data->Textures != nullptr)
            {
                for (ImTextureData* texture : *data->Textures)
                {
                    if (texture->Status != ImTextureStatus_OK)
                    {
                        texture->SetTexID(1);
                        texture->SetStatus(ImTextureStatus_OK);
                    }
                }
            }
        }

        // Parks the pointer mid-screen before a dialog is raised. A non-modal popup is placed at
        // the pointer, so leaving it at its never-set default would clamp the popup into the
        // top-left corner — where the click-on-void below would land inside it instead.
        void ParkPointer() { ImGui::GetIO().AddMousePosEvent(900.0f, 500.0f); }

        // A press-and-release on void, far from the dialog. The transition is what
        // ImGui reads as a click, so press and release land in different frames.
        template <typename Body>
        void ClickOnVoid(Body&& body)
        {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(4.0f, 4.0f);
            Frame(body);
            io.AddMouseButtonEvent(0, true);
            Frame(body);
            io.AddMouseButtonEvent(0, false);
            Frame(body);
            Frame(body);
        }

    private:
        ImGuiContext* m_Context = nullptr;
    };
}

TEST_CASE("a Modal survives a click on void; a Popup does not")
{
    HeadlessImGui imgui;
    bool bodyDrawn = false;

    SUBCASE("Popup")
    {
        // Reset per frame: only the last frame's verdict matters, and the click spans several.
        auto draw = [&]
        {
            bodyDrawn = false;
            if (auto popup = UI::Popup("##dialog"))
            {
                bodyDrawn = true;
            }
        };

        imgui.ParkPointer();
        imgui.Frame(
            [&]
            {
                UI::OpenPopup("##dialog");
                draw();
            });
        REQUIRE(bodyDrawn);

        imgui.ClickOnVoid(draw);
        CHECK_FALSE(bodyDrawn);
    }

    SUBCASE("Modal")
    {
        auto draw = [&]
        {
            bodyDrawn = false;
            if (auto modal = UI::Modal("Dialog##dialog"))
            {
                bodyDrawn = true;
            }
        };

        imgui.ParkPointer();
        imgui.Frame(
            [&]
            {
                UI::OpenPopup("Dialog##dialog");
                draw();
            });
        REQUIRE(bodyDrawn);

        imgui.ClickOnVoid(draw);
        CHECK(bodyDrawn);
    }
}

TEST_CASE("CloseCurrentPopup is the Modal's way out")
{
    HeadlessImGui imgui;
    bool bodyDrawn = false;
    bool answer = false;

    auto draw = [&]
    {
        bodyDrawn = false;
        if (auto modal = UI::Modal("Dialog##dialog"))
        {
            bodyDrawn = true;
            if (answer)
            {
                UI::CloseCurrentPopup();
            }
        }
    };

    imgui.Frame(
        [&]
        {
            UI::OpenPopup("Dialog##dialog");
            draw();
        });
    REQUIRE(bodyDrawn);

    // The frame the caller answers still draws the body; the next one does not.
    answer = true;
    imgui.Frame(draw);
    CHECK(bodyDrawn);

    imgui.Frame(draw);
    CHECK_FALSE(bodyDrawn);
}
