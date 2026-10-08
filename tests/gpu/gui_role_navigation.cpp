// Gui focus navigation from role actions, through the production stack (GPU band): keys land in a
// headless snapshot through InputRouter::Dispatch, the engine's role resolver resolves a UI input
// map's role actions each frame for the implicit seat and every SeatInput seat, and the real
// Gui::GuiConsumer drives Document::Navigate in the pressing seat's viewports. GPU only because the
// consumer walks Renderer::Viewport and Viewport::Create needs a live Context.
//
// The cases pin:
//   (a) the implicit seat drives the documents of viewports bound to no seat, and a seat drives only
//       its own viewports, each press reaching a document once;
//   (b) one press reaches exactly one document, the topmost that takes it;
//   (c) the modifiers held on the seat's keyboard reach Navigate, so Shift with a direction extends
//       an Extended list's range and Control moves focus alone.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/GuiConsumer.h>
#include <Veng/Gui/InputEvent.h>
#include <Veng/Input.h>
#include <Veng/Input/Actions.h>
#include <Veng/InputEvents.h>
#include <Veng/InputRouter.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

#include <Input/RoleResolver.h>
#include <gpu/fixture.h>

#include <vector>

using namespace Veng;

namespace
{
    // One row a list repeats its item template over.
    struct NavTrack
    {
        string Title;
    };

    // The array the list binds.
    struct NavPlaylist
    {
        vector<NavTrack> Tracks;
    };

    // The four directions on the arrows, no repeat: each tap is one step.
    Ref<InputMappingContext> MakeUiMap()
    {
        const auto action = [](const u64 id, const ActionRole role)
        {
            return InputAction{
                .Id = ActionId{id}, .Name = "ui", .Kind = ActionKind::Button, .Role = role};
        };
        const auto key = [](const u64 id, const Key code)
        {
            return Binding{.Source = {.Device = InputDeviceType::Keyboard, .Control = u32(code)},
                           .Action = ActionId{id}};
        };
        return InputMappingContext::Create(
            {action(1, ActionRole::NavigateUp), action(2, ActionRole::NavigateDown),
             action(3, ActionRole::NavigateLeft), action(4, ActionRole::NavigateRight)},
            {key(1, Key::Up), key(2, Key::Down), key(3, Key::Left), key(4, Key::Right)});
    }

    // The production stack over any number of viewports.
    struct NavRig
    {
        NavRig(Renderer::Context& context, AssetManager& assets)
            : GpuContext(context), Assets(assets),
              Router(nullptr, Snapshot, context.GetViewportRegistry()),
              Consumer(Router, Snapshot, nullptr, Viewports),
              Runner(WorldRunnerInfo{
                  .Types = &WorldTypes, .Systems = &Systems, .Presentation = &Presentation})
        {
            RegisterBuiltinTypes(WorldTypes);
            Router.RegisterConsumer(Consumer);
        }

        // A new offscreen viewport, last in the drive list (so topmost).
        Renderer::Viewport& AddViewport()
        {
            Owned.push_back(Renderer::Viewport::Create({
                .Context = GpuContext,
                .Assets = Assets,
                .Region = {.Offset = {0, 0}, .Extent = {128, 128}},
                .Settings = {},
                .Role = Renderer::ViewportRole::Offscreen,
            }));
            Viewports.push_back(Owned.back().get());
            return *Owned.back();
        }

        void Frame()
        {
            const PointerRouting pointer{};
            Resolver.Update(RoleFrameInfo{.Snapshot = Snapshot,
                                          .Router = Router,
                                          .Worlds = Runner,
                                          .DefaultUi = UiMap.get(),
                                          .Pointer = pointer,
                                          .Delta = 1.0f / 60.0f});
            // The next frame begins: the snapshot rolls, applying any release this frame deferred.
            Snapshot.BeginFrame(true);
        }

        void Hold(Key key)
        {
            KeyPressedEvent event(key, 0, 0);
            Router.Dispatch(event);
            Frame();
        }

        void Release(Key key)
        {
            KeyReleasedEvent event(key, 0, 0);
            Router.Dispatch(event);
            Frame();
        }

        void Press(Key key)
        {
            Hold(key);
            Release(key);
        }

        Renderer::Context& GpuContext;
        AssetManager& Assets;
        Input Snapshot{nullptr};
        InputRouter Router;
        std::vector<Renderer::Viewport*> Viewports;
        std::vector<Unique<Renderer::Viewport>> Owned;
        Gui::GuiConsumer Consumer;
        TypeRegistry WorldTypes;
        SystemRegistry Systems;
        PresentationScopes Presentation;
        WorldRunner Runner;
        RoleResolver Resolver;
        Ref<InputMappingContext> UiMap = MakeUiMap();
    };

    // An interactive document of three buttons in a row, the leftmost focused.
    struct ButtonRow
    {
        Gui::Document Doc;
        std::vector<Gui::Element*> Buttons;

        ButtonRow()
        {
            Doc.SetInteractive(true);
            for (int i = 0; i < 3; ++i)
            {
                Gui::Element& button = Doc.Add(Doc.Root(), Gui::ElementKind::Button);
                Doc.InitWidget(button);
                button.Layout =
                    Gui::Rect{.Min = {static_cast<f32>(i) * 60.0f, 0.0f}, .Size = {40.0f, 20.0f}};
                Buttons.push_back(&button);
            }
            Doc.SetFocus(Buttons[0]);
        }

        [[nodiscard]] usize Focused() const
        {
            for (usize i = 0; i < Buttons.size(); ++i)
            {
                if (Doc.GetFocused() == Buttons[i])
                {
                    return i;
                }
            }
            return Buttons.size();
        }
    };
}

VE_REFLECT(::NavTrack, 0xC1F9D3310DF65E76ULL)
VE_FIELD(Title)
VE_REFLECT_END();

VE_REFLECT(::NavPlaylist, 0x1D00BB95F3FF5D86ULL)
VE_ARRAY_FIELD(Tracks)
VE_REFLECT_END();

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "gui role navigation: the implicit seat drives unseated viewports, a seat its own")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());
    NavRig rig(Context, assets);

    // A world holding one seat that does not read the keyboard yet.
    const WorldInstanceId world = rig.Runner.OpenWorld(WorldOpenInfo{.StartSimulation = false});
    Scene& scene = rig.Runner.ResolveWorld(world)->GetScene();
    const Entity viewer = scene.CreateEntity();
    scene.Add<Viewer>(viewer);
    scene.Add<SeatInput>(viewer, SeatInput{.UsesKeyboardMouse = false});

    ButtonRow unseated;
    ButtonRow seated;
    rig.AddViewport().AttachDocument(unseated.Doc);
    Renderer::Viewport& seatView = rig.AddViewport();
    seatView.SetSeat(SeatRef{.World = world, .Viewer = viewer});
    seatView.AttachDocument(seated.Doc);

    // The implicit seat's press reaches the unseated document only, though the seated one sits on
    // top: a seated document is its seat's alone.
    rig.Press(Key::Right);
    CHECK(unseated.Focused() == 1);
    CHECK(seated.Focused() == 0);

    // Once the seat reads the keyboard, one press moves each document one step — the implicit seat's
    // in the unseated, the seat's in its own.
    scene.Get<SeatInput>(viewer).UsesKeyboardMouse = true;
    rig.Press(Key::Right);
    CHECK(unseated.Focused() == 2);
    CHECK(seated.Focused() == 1);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui role navigation: one press reaches exactly one document, the topmost")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());
    NavRig rig(Context, assets);

    ButtonRow below;
    ButtonRow above;
    Renderer::Viewport& view = rig.AddViewport();
    view.AttachDocument(below.Doc, 0);
    view.AttachDocument(above.Doc, 1);

    rig.Press(Key::Right);
    CHECK(above.Focused() == 1);
    CHECK(below.Focused() == 0);

    // Taken by neither (the top row cannot move further right), the press falls through to the next
    // document down rather than reaching both.
    rig.Press(Key::Right);
    rig.Press(Key::Right);
    CHECK(above.Focused() == 2);
    CHECK(below.Focused() == 1);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "gui role navigation: Shift extends an Extended list's range, Control moves alone")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());
    NavRig rig(Context, assets);

    Gui::Document doc;
    doc.SetInteractive(true);
    doc.Root().Layout = Gui::Rect{.Min = {0.0f, 0.0f}, .Size = {200.0f, 200.0f}};
    Gui::Element& list = doc.Add(doc.Root(), Gui::ElementKind::List);
    list.Bindings["items"] = "Tracks";
    list.Bindings["selection"] = "extended";
    doc.InitWidget(list);
    Gui::Element& row = doc.Add(list, Gui::ElementKind::Panel);
    Gui::Element& title = doc.Add(row, Gui::ElementKind::Text);
    title.Bindings["text"] = "Title";

    TypeRegistry registry;
    registry.Register<NavPlaylist>();
    NavPlaylist model{.Tracks = {{"a"}, {"b"}, {"c"}, {"d"}}};
    Gui::BindingContext context;
    context.SetData(model);
    doc.BindContext(&context, &registry);
    doc.UpdateBindings();
    for (usize i = 0; i < list.Children.size(); ++i)
    {
        Gui::Element& item = *list.Children[i];
        item.Layout =
            Gui::Rect{.Min = {0.0f, static_cast<f32>(i) * 40.0f}, .Size = {200.0f, 40.0f}};
        for (Gui::Element* child : item.Children)
        {
            child->Layout = Gui::Rect{.Min = item.Layout.Min, .Size = {100.0f, 40.0f}};
        }
    }
    rig.AddViewport().AttachDocument(doc);

    const auto selection = [&]
    {
        const std::span<const u32> items = doc.GetSelectedItems(list);
        return vector<u32>(items.begin(), items.end());
    };

    // A click on the first row selects it and sets the range anchor.
    Gui::PointerEvent down{.Kind = Gui::PointerEventKind::Down, .Position = {50.0f, 20.0f}};
    doc.DispatchPointer(down);
    Gui::PointerEvent up{.Kind = Gui::PointerEventKind::Up, .Position = {50.0f, 20.0f}};
    doc.DispatchPointer(up);
    REQUIRE(selection() == vector<u32>{0});

    // Shift held on the keyboard: each step down extends the range from the anchor.
    rig.Hold(Key::LeftShift);
    rig.Press(Key::Down);
    CHECK(selection() == vector<u32>{0, 1});
    rig.Press(Key::Down);
    CHECK(selection() == vector<u32>{0, 1, 2});
    rig.Release(Key::LeftShift);

    // Control held: focus travels without disturbing the selection.
    rig.Hold(Key::RightControl);
    rig.Press(Key::Down);
    rig.Release(Key::RightControl);
    CHECK(doc.GetFocused() == list.Children[3]);
    CHECK(selection() == vector<u32>{0, 1, 2});
}
