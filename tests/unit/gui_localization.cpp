// The Gui translation seam: a loc-keyed markup element resolves through a document's translator, a
// translator generation bump re-resolves and re-measures it, a null translator renders the key, and
// a LocKey-typed bound leaf shows the resolved string and re-resolves on a language change. Device-
// free — a fake GuiTranslator stands in for the localization service (its own resolution is covered
// by localization_service.cpp), and a text measurer sizes the leaf with no font resource; the one
// Instantiate case builds a cooked recipe over a headless AssetManager, no on-disk pack.

#include <map>
#include <string>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/GuiTranslator.h>
#include <Veng/Gui/UIDocument.h>
#include <Veng/Localization/LocKey.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Task/TaskSystem.h>

using namespace Veng;
using namespace Veng::Gui;

// A reflected view-model whose one field carries a translation key rather than a display string.
// Global scope — the reflect macros require the fully-qualified `::` spelling.
struct NameModel
{
    Veng::Localization::LocKey Name;
};

VE_REFLECT(::NameModel, 0x10C6E9CB4F2A0001ULL)
VE_FIELD(Name)
VE_REFLECT_END();

namespace
{
    // A GuiTranslator over two hand-built locale tables, with a generation that moves on a switch —
    // the document seam under test, isolated from the real service.
    class FakeTranslator final : public GuiTranslator
    {
    public:
        [[nodiscard]] std::string_view Translate(std::string_view key) const override
        {
            const auto it = m_Active->find(std::string(key));
            return it != m_Active->end() ? std::string_view(it->second) : key;
        }

        [[nodiscard]] u32 Generation() const override { return m_Generation; }

        // Switches the active locale table and bumps the generation, as SetLocale does on the service.
        void SetActive(const std::map<std::string, std::string>& table)
        {
            m_Active = &table;
            ++m_Generation;
        }

    private:
        const std::map<std::string, std::string>* m_Active = &m_Empty;
        std::map<std::string, std::string> m_Empty;
        u32 m_Generation = 0;
    };

    const std::map<std::string, std::string> English{{"greeting", "Hello"}};
    const std::map<std::string, std::string> French{{"greeting", "Bonjour"}};

    // A cooked recipe of one loc-keyed Text child under a Panel root — the markup `<Text loc="…">`
    // form as the loader hands it to Instantiate.
    Ref<UIDocument> LocKeyedRecipe()
    {
        const UIElementRecipe root{.Kind = ElementKind::Panel, .ChildCount = 1};
        const UIElementRecipe text{.Kind = ElementKind::Text, .Text = "greeting", .IsLocKey = true};
        return UIDocument::Create({root, text}, {}, {});
    }

    // Sizes a text leaf's height by its codepoint count so a re-measure is observable with no font.
    // Height is the column's main axis, so the leaf hugs it (the cross-axis width stretches to fill).
    void InstallLengthMeasurer(Document& doc)
    {
        doc.SetTextMeasurer([](std::string_view text, const Style&, optional<f32>)
                            { return vec2(100.0f, static_cast<f32>(text.size()) * 10.0f); });
    }
}

TEST_CASE("gui localization: a loc-keyed markup element resolves through the translator")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager assets(context, tasks, types);

    const Ref<UIDocument> recipe = LocKeyedRecipe();
    const Unique<Document> doc = Document::Instantiate(*recipe, assets);
    Element* const label = doc->Root().Children.at(0);

    // Instantiate retains the key and, with no translator, renders it (never a blank).
    CHECK(label->LocKey == "greeting");
    CHECK(label->Text == "greeting");

    FakeTranslator translator;
    translator.SetActive(English);
    doc->SetTranslator(&translator);

    // The presented text is the active-locale message; the authored key is still the element's own.
    CHECK(label->Text == "Hello");
    CHECK(label->LocKey == "greeting");

    // A null translator resolves the key to itself.
    doc->SetTranslator(nullptr);
    CHECK(label->Text == "greeting");
}

TEST_CASE("gui localization: a generation bump re-resolves and re-measures a loc-keyed element")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager assets(context, tasks, types);

    const Ref<UIDocument> recipe = LocKeyedRecipe();
    const Unique<Document> doc = Document::Instantiate(*recipe, assets);
    Element* const label = doc->Root().Children.at(0);
    InstallLengthMeasurer(*doc);

    FakeTranslator translator;
    translator.SetActive(English);
    doc->SetTranslator(&translator);
    doc->Solve(vec2(400.0f, 200.0f));
    CHECK(label->Text == "Hello");
    // "Hello" is five glyphs at 10px each of measured height.
    CHECK(label->Layout.Size.y == doctest::Approx(50.0f));

    // A language change moves the generation but touches no bound object; the per-frame UpdateBindings
    // path re-resolves the loc-key and the box follows the longer string's measure.
    translator.SetActive(French);
    doc->UpdateBindings();
    doc->Solve(vec2(400.0f, 200.0f));
    CHECK(label->Text == "Bonjour");
    CHECK(label->Layout.Size.y == doctest::Approx(70.0f));
}

TEST_CASE("gui localization: a markup paragraph's loc-keyed span re-measures its paragraph")
{
    Renderer::Context context;
    TaskSystem tasks;
    TypeRegistry types;
    AssetManager assets(context, tasks, types);

    // `<Text><Text loc="greeting"/><Text> world</Text></Text>` as the loader hands it over.
    const UIElementRecipe root{.Kind = ElementKind::Panel, .ChildCount = 1};
    const UIElementRecipe paragraph{.Kind = ElementKind::Text, .ChildCount = 2};
    const UIElementRecipe lead{.Kind = ElementKind::Text, .Text = "greeting", .IsLocKey = true};
    const UIElementRecipe body{.Kind = ElementKind::Text, .Text = " world"};
    const Ref<UIDocument> recipe = UIDocument::Create({root, paragraph, lead, body}, {}, {});
    const Unique<Document> doc = Document::Instantiate(*recipe, assets);
    Element* const line = doc->Root().Children.at(0);
    REQUIRE(line->Children.size() == 2);
    InstallLengthMeasurer(*doc);

    FakeTranslator translator;
    translator.SetActive(English);
    doc->SetTranslator(&translator);
    doc->Solve(vec2(400.0f, 200.0f));
    // "Hello world" is eleven glyphs, measured as one run.
    CHECK(line->Layout.Size.y == doctest::Approx(110.0f));

    translator.SetActive(French);
    doc->UpdateBindings();
    doc->Solve(vec2(400.0f, 200.0f));
    CHECK(line->Layout.Size.y == doctest::Approx(130.0f));
}

TEST_CASE("gui localization: a LocKey-typed bound leaf shows the resolved string")
{
    Document doc;
    Element& label = doc.Add(doc.Root(), ElementKind::Text);
    label.Bindings["text"] = "Name";

    NameModel model;
    model.Name.Key = "greeting";
    BindingContext binding;
    binding.SetData(model);
    TypeRegistry registry;
    registry.Register<Localization::LocKey>();
    registry.Register<NameModel>();
    doc.BindContext(&binding, &registry);

    FakeTranslator translator;
    translator.SetActive(English);
    doc.SetTranslator(&translator);

    doc.UpdateBindings();
    CHECK(label.Text == "Hello");

    // The leaf carries a key, so a language switch re-resolves it even though the model is unchanged.
    translator.SetActive(French);
    doc.UpdateBindings();
    CHECK(label.Text == "Bonjour");
}
