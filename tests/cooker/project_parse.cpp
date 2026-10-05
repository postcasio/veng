// project.veng parsing: the packs a project cooks, split into those the game mounts and those
// only the editor does.

#include <filesystem>
#include <fstream>
#include "support/TempPath.h"

#include <doctest/doctest.h>

#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

TEST_CASE("Cooker: ParseProject keeps editor-only packs apart from the game's")
{
    const path dir = TestSupport::TempDir() / "project_parse";
    std::filesystem::create_directories(dir);
    const path file = dir / "project.veng";
    std::ofstream(file) << R"({
  "packs": ["assets/game.vengpack.json"],
  "editorPacks": ["assets/editor.vengpack.json"],
  "configurations": ["configs/host.buildcfg"],
  "activeConfiguration": "host"
})";

    const Result<CookProject> project = ParseProject(file);
    REQUIRE(project.has_value());
    CHECK(project->Packs == vector<path>{dir / "assets/game.vengpack.json"});
    CHECK(project->EditorPacks == vector<path>{dir / "assets/editor.vengpack.json"});
}

TEST_CASE("Cooker: ParseProject treats a project with no editor packs as having none")
{
    const path dir = TestSupport::TempDir() / "project_parse_none";
    std::filesystem::create_directories(dir);
    const path file = dir / "project.veng";
    std::ofstream(file) << R"({"packs": ["a.vengpack.json"], "configurations": []})";

    const Result<CookProject> project = ParseProject(file);
    REQUIRE(project.has_value());
    CHECK(project->EditorPacks.empty());
}

TEST_CASE("Cooker: ParseProject refuses an editorPacks that is not a list of paths")
{
    const path dir = TestSupport::TempDir() / "project_parse_bad";
    std::filesystem::create_directories(dir);
    const path file = dir / "project.veng";
    std::ofstream(file) << R"({"packs": [], "editorPacks": "editor.vengpack.json"})";

    CHECK_FALSE(ParseProject(file).has_value());
}

TEST_CASE("Cooker: ParseProject reads the default UI context, and none when it is absent")
{
    const path dir = TestSupport::TempDir() / "project_parse_ui";
    std::filesystem::create_directories(dir);
    const path file = dir / "project.veng";

    std::ofstream(file) << R"({"packs": [], "defaultUiContext": "0x00000000000000A1"})";
    const Result<CookProject> named = ParseProject(file);
    REQUIRE(named.has_value());
    CHECK(named->DefaultUiContext.Value == 0xA1u);

    std::ofstream(file) << R"({"packs": []})";
    const Result<CookProject> absent = ParseProject(file);
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->DefaultUiContext.IsValid());

    std::ofstream(file) << R"({"packs": [], "defaultUiContext": 161})";
    CHECK_FALSE(ParseProject(file).has_value());
}
