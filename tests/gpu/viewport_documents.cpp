// Viewport attached-document layers. A Gui::Document attaches to a Viewport on an ordered layer
// stack; the engine drives each attached document's per-frame pipeline (Update -> Solve at the
// region extent -> Build) inside Viewport::Render and composites the layers through GuiScenePass
// over the scene output. These pin the seam:
//   (a) the layer stack order — two documents on distinct layers composite bottom -> top;
//   (b) the engine-driven re-solve on a region resize — the documents re-solve at the new extent
//       with no consumer Solve call;
//   (c) destruction self-detach — dropping a document's Unique removes it from the stack with no
//       dangling pointer, and the viewport keeps rendering the remaining layer.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Gui/Document.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Viewport.h>

#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

// After the Veng headers, so Veng.h's GLM_FORCE_DEPTH_ZERO_TO_ONE is set before glm.
#include <glm/gtc/packing.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 Extent{64, 48};

    Ref<Mesh> PopulateCubeScene(Context& context, AssetManager& assets, Scene& scene)
    {
        const Ref<Mesh> cube =
            Mesh::BuildSync(context, Primitives::Cube(1.0f), "Viewport Doc Cube");
        const Entity entity = scene.CreateEntity();
        scene.Add<Transform>(entity);
        scene.Add<MeshRenderer>(entity).Mesh = assets.Adopt(cube);
        return cube;
    }

    CameraView FrontCamera(uvec2 extent)
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(45.0f),
                              static_cast<f32>(extent.x) / static_cast<f32>(extent.y), 0.1f,
                              100.0f);
        camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }

    // Builds a document whose root Panel fills the region and paints a single opaque quad. When
    // halfWidth is set the quad covers only the left half (an absolute 50%-wide overlay); otherwise
    // it covers the whole region. The colors are linear straight-alpha, alpha 1 (fully opaque).
    Unique<Gui::Document> MakeSolidDocument(vec4 color, bool halfWidth)
    {
        auto document = CreateUnique<Gui::Document>();
        Gui::Element& root = document->Root();

        Gui::Style rootStyle;
        rootStyle.Width = Gui::Length::Percent(100.0f);
        rootStyle.Height = Gui::Length::Percent(100.0f);
        document->SetStyle(root, rootStyle);

        Gui::Element& fill = document->Add(root, Gui::ElementKind::Panel);
        Gui::Style fillStyle;
        fillStyle.Position = Gui::PositionType::Absolute;
        fillStyle.Inset.Left = 0.0f;
        fillStyle.Inset.Top = 0.0f;
        fillStyle.Inset.Bottom = 0.0f;
        fillStyle.Width = Gui::Length::Percent(halfWidth ? 50.0f : 100.0f);
        fillStyle.Height = Gui::Length::Percent(100.0f);
        fillStyle.Background = color;
        document->SetStyle(fill, fillStyle);

        return document;
    }

    // Decodes an RGBA16Sfloat download's pixel at (x, y) into a clamped [0,1] RGB triple.
    vec3 SampleRgb(const vector<u8>& halfBytes, uvec2 extent, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(halfBytes.data());
        const usize base = (static_cast<usize>(y) * extent.x + x) * 4;
        return vec3(glm::clamp(glm::unpackHalf1x16(halves[base + 0]), 0.0f, 1.0f),
                    glm::clamp(glm::unpackHalf1x16(halves[base + 1]), 0.0f, 1.0f),
                    glm::clamp(glm::unpackHalf1x16(halves[base + 2]), 0.0f, 1.0f));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "viewport documents: two layers composite bottom -> top, a region resize "
                  "re-solves the documents, and dropping one self-detaches")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> cube = PopulateCubeScene(Context, assets, *scene);

    const Unique<Viewport> viewport = Viewport::Create({
        .Context = Context,
        .Assets = assets,
        .Region = {.Offset = {0, 0}, .Extent = Extent},
        .ColorFormat = Format::RGBA16Sfloat,
        .Role = ViewportRole::Offscreen,
    });
    viewport->SetViewState({.World = scene.get(), .Camera = FrontCamera(Extent), .Delta = 0.016f});

    // With no documents attached the output is the scene renderer's output directly (byte-identical
    // to a viewport that never hosted a document) — no GuiScenePass work.
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    CHECK(viewport->GetAttachedDocuments().empty());
    CHECK(viewport->GetOutput().get() == viewport->GetRenderer().GetOutput().get());

    // The bottom layer fills the whole region red; the top layer covers only the left half green. If
    // the stack composites bottom -> top, the left half of the UI is green and the right half red.
    const Unique<Gui::Document> bottom =
        MakeSolidDocument(vec4(1.0f, 0.0f, 0.0f, 1.0f), /*halfWidth=*/false);
    Unique<Gui::Document> top = MakeSolidDocument(vec4(0.0f, 1.0f, 0.0f, 1.0f), /*halfWidth=*/true);

    // Attach out of layer order to prove ordering is by layer, not attach order: the top document
    // (layer 1) attaches first, the bottom (layer 0) second, yet the stack orders bottom first.
    viewport->AttachDocument(*top, /*layer=*/1);
    viewport->AttachDocument(*bottom, /*layer=*/0);

    const std::span<Gui::Document* const> stack = viewport->GetAttachedDocuments();
    REQUIRE(stack.size() == 2);
    CHECK(stack[0] == bottom.get());
    CHECK(stack[1] == top.get());

    // Each attached document tracks its host viewport for self-detach.
    CHECK(bottom->GetHostViewport() == viewport.get());
    CHECK(top->GetHostViewport() == viewport.get());

    // With documents attached the output is the UI composite, not the raw scene output.
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    REQUIRE_FALSE(viewport->GetAttachedDocuments().empty());
    CHECK(viewport->GetOutput().get() != viewport->GetRenderer().GetOutput().get());
    CHECK(viewport->GetOutputHandle().IsValid());

    {
        const vector<u8> raw = viewport->GetOutput()->GetImage()->Download();
        REQUIRE(raw.size() == static_cast<usize>(Extent.x) * Extent.y * 8);

        // Left column is green (top layer over bottom); right column is red (bottom layer alone).
        const vec3 left = SampleRgb(raw, Extent, Extent.x / 4, Extent.y / 2);
        const vec3 right = SampleRgb(raw, Extent, (Extent.x * 3) / 4, Extent.y / 2);
        CHECK(left.g > 0.5f);
        CHECK(left.r < 0.5f);
        CHECK(right.r > 0.5f);
        CHECK(right.g < 0.5f);
    }

    // Engine-driven re-solve on resize: growing the region re-solves the documents at the new extent
    // with no consumer Solve call — the root's computed layout tracks the new region size.
    constexpr uvec2 resized{96, 72};
    viewport->SetRegion({.Offset = {0, 0}, .Extent = resized});
    viewport->SetViewState({.World = scene.get(), .Camera = FrontCamera(resized), .Delta = 0.016f});
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });

    CHECK(bottom->Root().Layout.Size.x == doctest::Approx(static_cast<f32>(resized.x)));
    CHECK(bottom->Root().Layout.Size.y == doctest::Approx(static_cast<f32>(resized.y)));
    CHECK(top->Root().Layout.Size.x == doctest::Approx(static_cast<f32>(resized.x)));

    // The composite output tracks the new extent.
    REQUIRE(viewport->GetOutput() != nullptr);
    CHECK(viewport->GetOutput()->GetImage()->GetWidth() == resized.x);
    CHECK(viewport->GetOutput()->GetImage()->GetHeight() == resized.y);

    // Destruction self-detach: capture the raw pointer, drop the top document's Unique, and confirm
    // the stack drops to the bottom layer alone with no dangling pointer left behind.
    const Gui::Document* droppedTop = top.get();
    top.reset();

    const std::span<Gui::Document* const> afterDrop = viewport->GetAttachedDocuments();
    REQUIRE(afterDrop.size() == 1);
    CHECK(afterDrop[0] == bottom.get());
    CHECK(afterDrop[0] != droppedTop);

    // The viewport keeps rendering the remaining (bottom) layer: the whole region is now red.
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    {
        const vector<u8> raw = viewport->GetOutput()->GetImage()->Download();
        const vec3 left = SampleRgb(raw, resized, resized.x / 4, resized.y / 2);
        const vec3 right = SampleRgb(raw, resized, (resized.x * 3) / 4, resized.y / 2);
        CHECK(left.r > 0.5f);
        CHECK(left.g < 0.5f);
        CHECK(right.r > 0.5f);
    }

    // Explicit detach clears the host back-reference and empties the stack.
    viewport->DetachDocument(*bottom);
    CHECK(viewport->GetAttachedDocuments().empty());
    CHECK(bottom->GetHostViewport() == nullptr);
}
