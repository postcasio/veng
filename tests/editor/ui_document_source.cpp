// The UI document editor's markup model and its explicit-save contract, without a frame.
//
// Every authoring mutation the panel performs is a text rewrite on UIDocumentSource, so the
// mutations and the "an edit reaches no file; Save does" rule are checkable here. The canvas, the
// outline, and the asset-chip are not: those need a live ImGui frame and a device.

#include <doctest/doctest.h>

#include "AssetSaveModel.h"
#include "panels/UIDocumentSource.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace Veng;
using namespace VengEditor;

namespace
{
    const char* const RootMarkup = "<Panel>\n  <Text>hi</Text>\n</Panel>\n";

    path TempFile(const string& name)
    {
        const path file = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove(file);
        return file;
    }

    void WriteFile(const path& file, string_view text)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    }

    string ReadFile(const path& file)
    {
        const std::ifstream in(file, std::ios::binary);
        std::ostringstream contents;
        contents << in.rdbuf();
        return contents.str();
    }
}

TEST_CASE("an edit rewrites the markup in memory and reaches no file")
{
    const path file = TempFile("veng_uidoc_edit.vui.xml");
    WriteFile(file, RootMarkup);

    UIDocumentSource source;
    REQUIRE(source.Load(file).has_value());

    bool dirty = false;
    if (source.Edit([](const string& text) { return AppendImage(text); }))
    {
        dirty = true;
    }

    CHECK(dirty);
    CHECK(source.GetText().find("<Image") != string::npos);
    // The authoring action alone must not touch the user's file.
    CHECK(ReadFile(file) == RootMarkup);

    u32 cooks = 0;
    const VoidResult saved =
        SaveAssetSource([&] { return source.Write(file); }, dirty, [&] { ++cooks; });

    REQUIRE(saved.has_value());
    CHECK_FALSE(dirty);
    CHECK(cooks == 1);
    CHECK(ReadFile(file) == source.GetText());

    std::filesystem::remove(file);
}

TEST_CASE("an abandoned edit leaves the markup untouched and marks nothing dirty")
{
    UIDocumentSource source;
    const path file = TempFile("veng_uidoc_abandon.vui.xml");
    WriteFile(file, RootMarkup);
    REQUIRE(source.Load(file).has_value());

    CHECK_FALSE(source.Edit([](const string&) { return std::nullopt; }));
    // An edit that returns the text unchanged is not a change either.
    CHECK_FALSE(source.Edit([](const string& text) { return text; }));
    CHECK(source.GetText() == RootMarkup);

    std::filesystem::remove(file);
}

TEST_CASE("loading a missing source reports the failure rather than clearing the markup")
{
    UIDocumentSource source;
    const path missing = TempFile("veng_uidoc_absent.vui.xml");

    const VoidResult loaded = source.Load(missing);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().find(missing.string()) != string::npos);
}

TEST_CASE("AppendImage inserts before the outermost closing tag")
{
    const optional<string> edited = AppendImage(RootMarkup);
    REQUIRE(edited.has_value());

    const usize image = edited->find("<Image");
    const usize close = edited->rfind("</Panel>");
    REQUIRE(image != string::npos);
    REQUIRE(close != string::npos);
    CHECK(image < close);
    // The existing tree survives the insertion.
    CHECK(edited->find("<Text>hi</Text>") != string::npos);
}

TEST_CASE("AppendImage abandons markup carrying no closing tag")
{
    CHECK_FALSE(AppendImage("<Panel/>").has_value());
}

TEST_CASE("SetNthImageSrc inserts a src the tag lacks")
{
    const optional<string> edited = SetNthImageSrc("<Panel>\n  <Image/>\n</Panel>\n", 0, "0xAB");
    REQUIRE(edited.has_value());
    CHECK(edited->find("<Image src=\"0xAB\"/>") != string::npos);
}

TEST_CASE("SetNthImageSrc replaces the src of the addressed tag only")
{
    const string markup = "<Panel>\n  <Image src=\"0x01\"/>\n  <Image src=\"0x02\"/>\n</Panel>\n";

    const optional<string> second = SetNthImageSrc(markup, 1, "0xFF");
    REQUIRE(second.has_value());
    CHECK(second->find("src=\"0x01\"") != string::npos);
    CHECK(second->find("src=\"0xFF\"") != string::npos);
    CHECK(second->find("src=\"0x02\"") == string::npos);

    const optional<string> first = SetNthImageSrc(markup, 0, "0xFF");
    REQUIRE(first.has_value());
    CHECK(first->find("src=\"0xFF\"") != string::npos);
    CHECK(first->find("src=\"0x02\"") != string::npos);
}

TEST_CASE("SetNthImageSrc abandons an ordinal past the last Image tag")
{
    CHECK_FALSE(SetNthImageSrc("<Panel>\n  <Image/>\n</Panel>\n", 1, "0xAB").has_value());
}

TEST_CASE("SetNthImageSrc abandons an unterminated start tag")
{
    CHECK_FALSE(SetNthImageSrc("<Panel>\n  <Image src=\"0x01\"\n", 0, "0xAB").has_value());
}
