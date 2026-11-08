// The Metal backend's pipeline archive: a context given a pipeline cache path records every pipeline
// its compiler builds and writes them at teardown, and the next context given the path builds the
// same pipelines from that archive instead of compiling them. Reads the backend's own counters, so
// it registers only in a Metal tree.

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/ShaderModule.h>
#include <Veng/Task/TaskSystem.h>

#include <Renderer/Backend/Metal/Natives.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr AssetId RasterVertexId{0xB5B397C02C4C929DULL};
    constexpr AssetId RasterFragmentId{0x396E60A65194A327ULL};
    constexpr AssetId InvertComputeId{0x1F41};

    struct ArchiveCounts
    {
        u64 Hits = 0;
        u64 Builds = 0;
        /// The file the backend keeps its archive in.
        path Archive;
    };

    // Brings a context up on @p archive, builds one graphics and one compute pipeline from the
    // test pack, and reports how the context's pipelines came to be; teardown writes the archive.
    ArchiveCounts BuildPipelines(const path& archive)
    {
        Context context;
        context.Initialize({.ApplicationName = "veng_pipeline_archive",
                            .HeadlessExtent = {4, 4},
                            .PipelineCachePath = archive},
                           nullptr);
        TaskSystem tasks{TaskSystemInfo{.WorkerCount = 1}};
        TypeRegistry types;
        {
            AssetManager assets(context, tasks, types);
            REQUIRE(assets.Mount(path(TEST_SHADER_PACK)));
            const auto vertex = assets.LoadSync<Shader>(RasterVertexId);
            const auto fragment = assets.LoadSync<Shader>(RasterFragmentId);
            const auto compute = assets.LoadSync<Shader>(InvertComputeId);
            REQUIRE(vertex);
            REQUIRE(fragment);
            REQUIRE(compute);

            const auto layout = PipelineLayout::Create(
                context,
                {.Name = "Archive Layout",
                 .PushConstantRanges = {{.Stages = ShaderStage::Vertex | ShaderStage::Fragment,
                                         .Offset = 0,
                                         .Size = 64}}});
            const auto graphics = GraphicsPipeline::Create(
                context,
                {.Name = "Archive Graphics",
                 .ColorAttachments = {{.Format = Format::RGBA8Unorm}},
                 .PipelineLayout = layout,
                 .ShaderStages = {
                     {.Stage = ShaderStage::Vertex, .Module = vertex->Get()->Module},
                     {.Stage = ShaderStage::Fragment, .Module = fragment->Get()->Module}}});
            const auto computeLayout =
                PipelineLayout::Create(context, {.Name = "Archive Compute Layout"});
            const auto kernel = ComputePipeline::Create(
                context,
                {.Name = "Archive Compute",
                 .PipelineLayout = computeLayout,
                 .ShaderStage = {.Stage = ShaderStage::Compute, .Module = compute->Get()->Module}});
            REQUIRE(graphics != nullptr);
            REQUIRE(kernel != nullptr);
        }
        const Context::Native& native = context.GetNative();
        return {.Hits = native.ArchiveHits,
                .Builds = native.ArchiveBuilds,
                .Archive = native.ArchivePath};
    }
}

TEST_CASE("pipeline archive: a second context builds its pipelines from the archive the first "
          "wrote")
{
    const path directory = std::filesystem::temp_directory_path() / "veng_pipeline_archive_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const path archive = directory / "pipelines.bin";

    const ArchiveCounts cold = BuildPipelines(archive);
    CHECK(cold.Hits == 0);
    CHECK(cold.Builds >= 2);
    // The backend keeps a file of its own beside the path, which another backend may also use.
    CHECK(cold.Archive != archive);
    CHECK(cold.Archive.parent_path() == archive.parent_path());
    REQUIRE(std::filesystem::exists(cold.Archive));

    // Under shader validation every pipeline is built instrumented, which no archive holds.
    if (std::getenv("MTL_SHADER_VALIDATION") != nullptr)
    {
        MESSAGE("shader validation builds every pipeline instrumented; the archive serves none");
        std::filesystem::remove_all(directory);
        return;
    }
    const ArchiveCounts warm = BuildPipelines(archive);
    CHECK(warm.Builds == 0);
    CHECK(warm.Hits == cold.Builds);

    std::filesystem::remove_all(directory);
}

TEST_CASE("pipeline archive: a file that is not a Metal archive is ignored, not read")
{
    const path directory =
        std::filesystem::temp_directory_path() / "veng_pipeline_archive_foreign_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    const path cachePath = directory / "pipelines.bin";

    // A pipeline-cache header of the kind another backend writes, then a body, at both the cache
    // path and the file this backend reads.
    const vector<u8> foreign = [&]
    {
        vector<u8> bytes(256, 0x5A);
        const u32 header[] = {32, 1, 0x106B, 0x1A05020A};
        std::memcpy(bytes.data(), header, sizeof(header));
        return bytes;
    }();
    const auto write = [&](const path& file)
    {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(foreign.data()),
                  static_cast<std::streamsize>(foreign.size()));
    };
    const path backendFile = directory / "pipelines.metal.bin";
    write(cachePath);
    write(backendFile);

    const ArchiveCounts counts = BuildPipelines(cachePath);
    CHECK(counts.Hits == 0);
    CHECK(counts.Builds >= 2);
    CHECK(counts.Archive == backendFile);

    // The cache path is never touched.
    std::ifstream kept(cachePath, std::ios::binary);
    const vector<u8> keptBytes((std::istreambuf_iterator<char>(kept)),
                               std::istreambuf_iterator<char>());
    CHECK(keptBytes == foreign);

    // The backend's own file now holds an archive the next context builds from.
    if (std::getenv("MTL_SHADER_VALIDATION") == nullptr)
    {
        const ArchiveCounts warm = BuildPipelines(cachePath);
        CHECK(warm.Builds == 0);
        CHECK(warm.Hits == counts.Builds);
    }

    std::filesystem::remove_all(directory);
}
