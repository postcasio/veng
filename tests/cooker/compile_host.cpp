// vengc compile-host's refusals and its depfile, through the in-process HostShaderCompiler. The
// cases share one compiler, since its Slang global session is the costly part.

#include <fstream>
#include <sstream>

#include <doctest/doctest.h>

#include <Veng/Cook/HostShaderModule.h>

#include <support/TempPath.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    HostShaderCompiler& SharedCompiler()
    {
        static HostShaderCompiler compiler = []
        {
            Result<HostShaderCompiler> created = HostShaderCompiler::Create();
            REQUIRE_MESSAGE(created, created.error());
            return std::move(*created);
        }();
        return compiler;
    }

    path HostFixture(const char* name)
    {
        return path(VENG_COOKER_TEST_FIXTURE_DIR) / "host" / name;
    }

    Result<HostShaderModuleOutput> CompileFixture(const char* name)
    {
        return SharedCompiler().Compile({.Module = HostFixture(name), .Name = "Fixture"});
    }
}

TEST_CASE("compile-host refuses a module declaring a resource, naming it")
{
    const Result<HostShaderModuleOutput> compiled = CompileFixture("texture_param.slang");
    REQUIRE_FALSE(compiled);
    CHECK(compiled.error().find("texture_param.slang") != string::npos);
    CHECK(compiled.error().find("'u_Albedo'") != string::npos);
}

TEST_CASE("compile-host refuses a struct whose field cannot cross, naming the field")
{
    const Result<HostShaderModuleOutput> compiled = CompileFixture("struct_field.slang");
    REQUIRE_FALSE(compiled);
    CHECK(compiled.error().find("'Fade'") != string::npos);
    CHECK(compiled.error().find("'Visibility.Visible'") != string::npos);
}

TEST_CASE("compile-host refuses an export returning an array")
{
    const Result<HostShaderModuleOutput> compiled = CompileFixture("array_return.slang");
    REQUIRE_FALSE(compiled);
    CHECK(compiled.error().find("'Pair'") != string::npos);
}

TEST_CASE("compile-host mirrors a struct in the header and passes aggregates by reference")
{
    const Result<HostShaderModuleOutput> compiled = CompileFixture("aggregate_params.slang");
    REQUIRE_MESSAGE(compiled, compiled.error());
    CHECK(compiled->Header.find("struct Span") != string::npos);
    CHECK(compiled->Header.find("float Low{};") != string::npos);
    CHECK(compiled->Header.find("float High{};") != string::npos);
    CHECK(compiled->Header.find("Span Widen(const Span& span, const std::array<glm::vec4, 4>& "
                                "values, std::array<float, 4>& sums)") != string::npos);
    // A struct `in` parameter also has a prepared form and an overload taking it.
    CHECK(compiled->Header.find("template <> class Prepared<Span>") != string::npos);
    CHECK(compiled->Header.find("Span Widen(const Prepared<Span>& span, const "
                                "std::array<glm::vec4, 4>& values, std::array<float, 4>& sums)") !=
          string::npos);
}

TEST_CASE("compile-host refuses a struct named as the prepared-parameter handle")
{
    const Result<HostShaderModuleOutput> compiled = CompileFixture("reserved_name.slang");
    REQUIRE_FALSE(compiled);
    CHECK(compiled.error().find("'Prepared'") != string::npos);
}

TEST_CASE("compile-host's depfile names an included file")
{
    const Result<HostShaderModuleOutput> compiled = CompileFixture("includes_helper.slang");
    REQUIRE_MESSAGE(compiled, compiled.error());

    const path outDir = TestSupport::TempDir() / "compile_host_depfile";
    const path depfile = outDir / "Fixture.d";
    const VoidResult written = WriteHostShaderModule(*compiled, "Fixture", outDir, depfile);
    REQUIRE_MESSAGE(written, written.error());

    const std::ifstream in(depfile);
    std::stringstream text;
    text << in.rdbuf();
    CHECK(text.str().find("Fixture.cpp:") != string::npos);
    CHECK(text.str().find("scale_helper.slang") != string::npos);
    // The header declares the export in its namespace, with the out parameter as a reference.
    CHECK(compiled->Header.find("namespace Fixture") != string::npos);
    CHECK(compiled->Header.find("glm::vec2 ScalePair(glm::vec2 v, float& sum)") != string::npos);
}
