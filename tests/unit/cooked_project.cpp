// CookedProject round-trip tests: pure, no GPU. Proves the .vengproj
// runtime-entrypoint writer/reader contract.

#include <doctest/doctest.h>
#include "support/TempPath.h"

#include <cstring>
#include <fstream>

#include <fmt/format.h>

#include <Veng/Asset/CookedProject.h>

using namespace Veng;

namespace
{
    // A unique temp path so the round-trip never collides with a concurrent test run.
    path TempProjectPath(const char* tag)
    {
        return Veng::TestSupport::TempDir() / fmt::format("veng_cooked_project_{}.vengproj", tag);
    }
}

TEST_CASE("CookedProject: round-trips the startup level, default UI context and pack names")
{
    const path file = TempProjectPath("roundtrip");

    CookedProject project;
    project.StartupLevel = AssetId{0xABCDEF0123456789ULL};
    project.DefaultUiContext = AssetId{0x0123456789ABCDEFULL};
    project.PackMountNames = {"sample.vengpack", "dlc.vengpack"};

    REQUIRE(WriteCookedProject(file, project).has_value());

    const Result<CookedProject> read = ReadCookedProject(file);
    REQUIRE(read.has_value());
    CHECK(read->StartupLevel.Value == 0xABCDEF0123456789ULL);
    CHECK(read->DefaultUiContext.Value == 0x0123456789ABCDEFULL);
    REQUIRE(read->PackMountNames.size() == 2);
    CHECK(read->PackMountNames[0] == "sample.vengpack");
    CHECK(read->PackMountNames[1] == "dlc.vengpack");

    std::filesystem::remove(file);
}

TEST_CASE("CookedProject: round-trips an empty pack list and no startup level")
{
    const path file = TempProjectPath("empty");

    REQUIRE(WriteCookedProject(file, CookedProject{}).has_value());

    const Result<CookedProject> read = ReadCookedProject(file);
    REQUIRE(read.has_value());
    CHECK_FALSE(read->StartupLevel.IsValid());
    CHECK_FALSE(read->DefaultUiContext.IsValid());
    CHECK(read->PackMountNames.empty());

    std::filesystem::remove(file);
}

TEST_CASE("ReadCookedProject: rejects a bad magic")
{
    const path file = TempProjectPath("badmagic");

    REQUIRE(WriteCookedProject(file, CookedProject{}).has_value());

    // Corrupt the magic in place; the reader must reject it.
    std::fstream raw(file, std::ios::binary | std::ios::in | std::ios::out);
    REQUIRE(raw.good());
    raw.seekp(0);
    const char junk = 'X';
    raw.write(&junk, 1);
    raw.close();

    const Result<CookedProject> read = ReadCookedProject(file);
    CHECK_FALSE(read.has_value());

    std::filesystem::remove(file);
}
