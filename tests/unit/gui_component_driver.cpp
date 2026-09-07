// Scoped binding contexts and handler routing for an embedded component's own driver: a
// `{obj.field}` binding and a named handler under a component boundary resolve against the nearest
// ancestor boundary's context (falling back to the document root when none binds one), each scoped
// context keeps its own version watermark, and a handler unresolved in the component falls back to
// the host table. Device-free — the scoped bind and the handler firing exercise the same paths a
// component driver's OnInstantiate + the event path drive, with no GPU, scene, or asset manager.

#include <doctest/doctest.h>

#include <Veng/Gui/Document.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>

using namespace Veng;
using namespace Veng::Gui;

// A one-field reflected view-model: the host and each embedded component bind their own instance,
// so a `{Label}` binding proves which context an element resolved against. Global scope — the
// reflect macros require the fully-qualified `::` spelling.
struct ScopedModel
{
    Veng::string Label;
};

VE_REFLECT(::ScopedModel, 0x51C0000000000001ULL)
VE_FIELD(Label)
VE_REFLECT_END();

TEST_CASE(
    "gui component: a scoped context resolves against the component, and does not shadow above")
{
    Document doc;

    // A text above any boundary reads the host context; a text inside a component boundary reads
    // the component's, even though the host context declares an identically-named field.
    Element& hostText = doc.Add(doc.Root(), ElementKind::Text);
    hostText.Bindings["text"] = "Label";
    Element& boundary = doc.Add(doc.Root(), ElementKind::Component);
    Element& innerText = doc.Add(boundary, ElementKind::Text);
    innerText.Bindings["text"] = "Label";

    TypeRegistry registry;
    registry.Register<ScopedModel>();

    ScopedModel host{.Label = "host"};
    ScopedModel component{.Label = "component"};
    BindingContext hostContext;
    hostContext.SetData(host);
    BindingContext componentContext;
    componentContext.SetData(component);

    doc.BindContext(&hostContext, &registry);
    doc.BindContext(doc.GetHandle(boundary), &componentContext, &registry);

    doc.UpdateBindings();
    CHECK(hostText.Text == "host");
    CHECK(innerText.Text == "component");

    // A scoped context binds only its subtree: clearing it leaves the host binding untouched and the
    // component subtree falls back to the host context.
    doc.BindContext(doc.GetHandle(boundary), nullptr, &registry);
    component.Label = "changed";
    componentContext.Invalidate();
    hostContext.Invalidate();
    doc.UpdateBindings();
    CHECK(hostText.Text == "host");
    CHECK(innerText.Text == "host");
}

TEST_CASE(
    "gui component: two embeds of one component get independent, separately-versioned contexts")
{
    Document doc;

    // Two component boundaries, each with its own scoped context — the split-screen property applied
    // to components: each embed is a distinct driver instance binding its own view-model.
    Element& boundaryA = doc.Add(doc.Root(), ElementKind::Component);
    Element& textA = doc.Add(boundaryA, ElementKind::Text);
    textA.Bindings["text"] = "Label";
    Element& boundaryB = doc.Add(doc.Root(), ElementKind::Component);
    Element& textB = doc.Add(boundaryB, ElementKind::Text);
    textB.Bindings["text"] = "Label";

    TypeRegistry registry;
    registry.Register<ScopedModel>();

    ScopedModel a{.Label = "A0"};
    ScopedModel b{.Label = "B0"};
    BindingContext contextA;
    contextA.SetData(a);
    BindingContext contextB;
    contextB.SetData(b);
    doc.BindContext(doc.GetHandle(boundaryA), &contextA, &registry);
    doc.BindContext(doc.GetHandle(boundaryB), &contextB, &registry);

    doc.UpdateBindings();
    CHECK(textA.Text == "A0");
    CHECK(textB.Text == "B0");

    // Mutating A's model without bumping its version is not read — each context re-reads only when
    // its own version moves, so the per-context watermark holds.
    a.Label = "A1";
    doc.UpdateBindings();
    CHECK(textA.Text == "A0");

    // Bumping only A's version re-reads A and leaves B untouched.
    contextA.Invalidate();
    doc.UpdateBindings();
    CHECK(textA.Text == "A1");
    CHECK(textB.Text == "B0");
}

TEST_CASE("gui component: an event resolves component-first, falling back to the host table")
{
    Document doc;

    Element& boundary = doc.Add(doc.Root(), ElementKind::Component);
    // A button whose handler the component defines, and one whose handler only the host defines.
    Element& componentButton = doc.Add(boundary, ElementKind::Button);
    componentButton.Bindings["onClick"] = "act";
    doc.InitWidget(componentButton);
    Element& fallbackButton = doc.Add(boundary, ElementKind::Button);
    fallbackButton.Bindings["onClick"] = "hostAction";
    doc.InitWidget(fallbackButton);
    // A button above the boundary fires the host table directly.
    Element& hostButton = doc.Add(doc.Root(), ElementKind::Button);
    hostButton.Bindings["onClick"] = "hostAction";
    doc.InitWidget(hostButton);

    const TypeRegistry registry;
    int componentActs = 0;
    int hostActs = 0;
    BindingContext hostContext;
    hostContext.SetHandler("hostAction", [&](Element&) { ++hostActs; });
    // The host also defines "act" — but the component's own "act" must win where the component
    // defines it, so this handler must not fire from inside the component.
    hostContext.SetHandler("act", [&](Element&) { ++hostActs; });
    BindingContext componentContext;
    componentContext.SetHandler("act", [&](Element&) { ++componentActs; });

    doc.BindContext(&hostContext, &registry);
    doc.BindContext(doc.GetHandle(boundary), &componentContext, &registry);
    doc.SetInteractive(true);

    // An onClick the component defines fires the component's handler, not the host's same-named one.
    doc.SetFocus(&componentButton);
    CHECK(doc.Navigate(NavAction::Confirm));
    CHECK(componentActs == 1);
    CHECK(hostActs == 0);

    // An onClick the component does not define falls back to the host table.
    doc.SetFocus(&fallbackButton);
    CHECK(doc.Navigate(NavAction::Confirm));
    CHECK(hostActs == 1);

    // A button above the boundary resolves against the host directly.
    doc.SetFocus(&hostButton);
    CHECK(doc.Navigate(NavAction::Confirm));
    CHECK(hostActs == 2);
    CHECK(componentActs == 1);
}

TEST_CASE("gui component: an empty or root boundary handle binds the whole-document context")
{
    Document doc;
    Element& text = doc.Add(doc.Root(), ElementKind::Text);
    text.Bindings["text"] = "Label";

    TypeRegistry registry;
    registry.Register<ScopedModel>();
    ScopedModel model{.Label = "whole"};
    BindingContext context;
    context.SetData(model);

    // A root/empty handle routes to the document context rather than a scoped entry, so a driver
    // written to scope its bind works unchanged as a whole-document overlay driver.
    doc.BindContext(doc.GetHandle(doc.Root()), &context, &registry);
    doc.UpdateBindings();
    CHECK(text.Text == "whole");
}
