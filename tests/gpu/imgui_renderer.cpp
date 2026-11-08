// ImGui renderer: the layer draws ImGui's lists through the engine's own pipeline. One frame draws a
// registered ImGuiTexture, an ImGui-owned dynamic texture, and a filled rectangle under a clip
// rectangle; a second frame updates the dynamic texture. Each texel probed is one whose colour
// exactly one of those three determines.

#include <doctest/doctest.h>

#include <array>
#include <cstring>

#include <Veng/Asset/AssetManager.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/TextureReadback.h>
#include <Veng/Task/TaskSystem.h>
#include <Veng/Vendor/ImGui.h>
#include <Veng/Vendor/ImGuiInternal.h>
#include <Veng/Window.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // Display-space rectangles, in ImGui units, each probed at its centre.
    constexpr vec2 RegisteredMin{8.0f, 8.0f};
    constexpr vec2 RegisteredMax{40.0f, 40.0f};
    constexpr vec2 DynamicMin{48.0f, 8.0f};
    constexpr vec2 DynamicMax{80.0f, 40.0f};
    // The filled rectangle spans both halves; the clip keeps only its left half.
    constexpr vec2 FillMin{8.0f, 56.0f};
    constexpr vec2 FillMax{72.0f, 88.0f};
    constexpr vec2 ClipMax{40.0f, 88.0f};

    constexpr f32 Tolerance = 0.02f;

    // Fills an ImGui texture's whole pixel store with one RGBA8 colour.
    void FillPixels(const ImTextureData& texture, const std::array<u8, 4>& rgba)
    {
        for (int i = 0; i < texture.Width * texture.Height; ++i)
        {
            std::memcpy(texture.Pixels + (static_cast<ptrdiff_t>(i) * 4), rgba.data(), 4);
        }
    }

    // Draws the three features into the foreground list of the open frame.
    void DrawFeatures(const Ref<ImGuiTexture>& registered, ImTextureData& dynamic)
    {
        ImDrawList* list = ImGui::GetForegroundDrawList();
        list->AddImage(static_cast<ImTextureID>(registered->GetTextureId()), RegisteredMin,
                       RegisteredMax);
        list->AddImage(dynamic.GetTexRef(), DynamicMin, DynamicMax);
        list->PushClipRect(FillMin, ClipMax);
        list->AddRectFilled(FillMin, FillMax, IM_COL32_WHITE);
        list->PopClipRect();
    }

    // Reads the UI image texel under a display-space point.
    vec4 TexelAt(const TexelReadback& readback, const vec2 display)
    {
        const vec2 scale = vec2(readback.Extent) / vec2(ImGui::GetIO().DisplaySize);
        const uvec2 texel = uvec2(display * scale);
        return readback.Texels[(static_cast<usize>(texel.y) * readback.Extent.x) + texel.x];
    }

    bool Near(const vec4 actual, const vec4 expected)
    {
        return glm::all(glm::lessThanEqual(glm::abs(actual - expected), vec4(Tolerance)));
    }
}

TEST_CASE("imgui renderer: registered, dynamic and clipped draws land where they are drawn")
{
    const Unique<Window> window = Window::Create({
        .Extent = {128, 128},
        .Resizable = false,
        .Title = "ImGui Renderer Test",
        .CaptureMouse = false,
    });

    Context context;
    context.Initialize({.ApplicationName = "ImGui Renderer Test"}, window.get());

    TaskSystem tasks{TaskSystemInfo{.WorkerCount = 2}};
    TypeRegistry types;
    {
        AssetManager assets(context, tasks, types);

        // Declared before the layer so it outlives the ImGui context that references it.
        ImTextureData dynamic;
        dynamic.Create(ImTextureFormat_RGBA32, 4, 4);
        FillPixels(dynamic, {255, 0, 0, 255});

        const Unique<ImGuiLayer> imgui = ImGuiLayer::Create({}, context, assets, *window);
        ImGui::RegisterUserTexture(&dynamic);

        const Ref<Image> blue =
            Image::Create(context, {
                                       .Name = "ImGui Registered Texture",
                                       .Extent = {4, 4, 1},
                                       .Format = Format::RGBA8Unorm,
                                       .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                                   });
        std::array<u8, 4 * 4 * 4> bluePixels{};
        for (usize i = 0; i < bluePixels.size(); i += 4)
        {
            bluePixels[i + 2] = 255;
            bluePixels[i + 3] = 255;
        }
        blue->UploadSync(bluePixels);
        const Ref<ImageView> blueView = ImageView::Create(context, {.Image = blue});
        const Ref<Sampler> sampler =
            Sampler::Create(context, {
                                         .Name = "ImGui Test Sampler",
                                         .MagFilter = Filter::Nearest,
                                         .MinFilter = Filter::Nearest,
                                         .AddressModeU = AddressMode::ClampToEdge,
                                         .AddressModeV = AddressMode::ClampToEdge,
                                         .AddressModeW = AddressMode::ClampToEdge,
                                     });
        const Ref<ImGuiTexture> registered = imgui->CreateTexture(*sampler, *blueView);
        CHECK(registered->GetTextureId() != 0);

        const auto renderFrame = [&]
        {
            imgui->BeginFrame();
            DrawFeatures(registered, dynamic);
            context.ImmediateCommands([&](CommandBuffer& cmd) { imgui->Render(cmd); });
            REQUIRE(imgui->HasDrawnOutput());
            Result<TexelReadback> readback =
                ReadbackTexels(context, assets, imgui->GetOutputImage());
            REQUIRE(readback.has_value());
            return std::move(*readback);
        };

        const TexelReadback first = renderFrame();
        CHECK(Near(TexelAt(first, (RegisteredMin + RegisteredMax) * 0.5f), {0, 0, 1, 1}));
        CHECK(Near(TexelAt(first, (DynamicMin + DynamicMax) * 0.5f), {1, 0, 0, 1}));
        CHECK(Near(TexelAt(first, {24.0f, 72.0f}), {1, 1, 1, 1}));
        CHECK(Near(TexelAt(first, {56.0f, 72.0f}), {0, 0, 0, 0}));

        // ImGui's own update protocol: the changed region, then the status.
        FillPixels(dynamic, {0, 255, 0, 255});
        const ImTextureRect whole{.x = 0, .y = 0, .w = 4, .h = 4};
        dynamic.Updates.push_back(whole);
        dynamic.UpdateRect = whole;
        dynamic.SetStatus(ImTextureStatus_WantUpdates);

        const TexelReadback second = renderFrame();
        CHECK(Near(TexelAt(second, (DynamicMin + DynamicMax) * 0.5f), {0, 1, 0, 1}));
        CHECK(Near(TexelAt(second, (RegisteredMin + RegisteredMax) * 0.5f), {0, 0, 1, 1}));

        ImGui::UnregisterUserTexture(&dynamic);
    }
}
