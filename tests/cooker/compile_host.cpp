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

TEST_CASE("compile-host refuses an export returning a struct")
{
    const Result<HostShaderModuleOutput> compiled = CompileFixture("struct_return.slang");
    REQUIRE_FALSE(compiled);
    CHECK(compiled.error().find("'MakeSpan'") != string::npos);
    CHECK(compiled.error().find("'Span'") != string::npos);
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
