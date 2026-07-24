// Guards the static/skinned surface vertex dedupe. The core Surface g-buffer pipeline builds two
// vertex stages from one material — surface.vert (static) and surface_skinned.vert (skinned) — that
// must write the identical fragment-input interpolants, or a skinned mesh silently renders wrong.
// The dedupe makes that structural: both verts #include Veng/surface_vertex.slang, output the
// SurfaceFragmentInput struct declared once in Veng/surface.slang, and share its transform, so
// neither can re-declare the contract. This source-text gate pins that arrangement — if a later
// edit stops using the shared include or re-declares a local output struct in a vert, drift fails
// loudly here rather than at runtime. Pure text over the source tree, no GPU.

#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>

#include <Veng/Veng.h>

namespace
{
    Veng::string ReadCoreShader(const Veng::string& relative)
    {
        const Veng::string full = Veng::string(VENG_CORE_SHADER_DIR) + "/" + relative;
        const std::ifstream file(full, std::ios::binary);
        REQUIRE_MESSAGE(file.good(), "cannot open core shader: ", full);
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // Number of non-overlapping occurrences of `needle` in `haystack`.
    Veng::usize CountOccurrences(const Veng::string& haystack, const Veng::string& needle)
    {
        Veng::usize count = 0;
        for (Veng::usize pos = haystack.find(needle); pos != Veng::string::npos;
             pos = haystack.find(needle, pos + needle.size()))
        {
            ++count;
        }
        return count;
    }
}

TEST_CASE("both surface verts include the shared vertex header")
{
    const Veng::string staticVert = ReadCoreShader("surface.vert.slang");
    const Veng::string skinnedVert = ReadCoreShader("surface_skinned.vert.slang");

    CHECK(staticVert.find("#include \"Veng/surface_vertex.slang\"") != Veng::string::npos);
    CHECK(skinnedVert.find("#include \"Veng/surface_vertex.slang\"") != Veng::string::npos);
}

TEST_CASE("neither surface vert re-declares the fragment-input contract")
{
    const Veng::string staticVert = ReadCoreShader("surface.vert.slang");
    const Veng::string skinnedVert = ReadCoreShader("surface_skinned.vert.slang");

    // A hand-maintained copy of the output struct (the exact drift this dedupe removes) reappears
    // as a local struct declaration. Neither vert may declare a fragment-output struct of its own.
    CHECK(staticVert.find("struct VSOutput") == Veng::string::npos);
    CHECK(skinnedVert.find("struct VSOutput") == Veng::string::npos);
    CHECK(staticVert.find("struct SurfaceFragmentInput") == Veng::string::npos);
    CHECK(skinnedVert.find("struct SurfaceFragmentInput") == Veng::string::npos);

    // Both entry points emit the shared struct, so the vert output and the fragment input are one
    // type by construction.
    CHECK(staticVert.find("SurfaceFragmentInput vsMain") != Veng::string::npos);
    CHECK(skinnedVert.find("SurfaceFragmentInput vsMain") != Veng::string::npos);
}

TEST_CASE("the surface fragment-input struct is declared in exactly one place")
{
    const Veng::string surfaceHeader = ReadCoreShader("Veng/surface.slang");
    const Veng::string sharedVertex = ReadCoreShader("Veng/surface_vertex.slang");

    // The single source of the interpolant contract is Veng/surface.slang; the shared vertex header
    // uses it but must not re-declare it.
    CHECK(CountOccurrences(surfaceHeader, "struct SurfaceFragmentInput") == 1);
    CHECK(sharedVertex.find("struct SurfaceFragmentInput") == Veng::string::npos);

    // The shared assembly the two verts hand off to lives here, once.
    CHECK(CountOccurrences(sharedVertex, "SurfaceFragmentInput AssembleSurfaceVertex") == 1);
}
