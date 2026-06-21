// Async build factories for Texture and Material: the Task<Ref<T>> siblings of
// Mesh::CreateAsync. Each builds a runtime resource off the render thread and
// streams it in through AssetManager::CreateAsync, proving the worker job yields
// a finalized, resident resource the main-thread continuation pump publishes.
//
// The Texture case uploads CPU pixels through the transfer queue and asserts the
// streamed-in texture has the expected extent/format and a valid bindless handle.
// The Material case assembles a runtime parameter block, streams the material in,
// then draws a fullscreen triangle whose fragment shader reads the block back —
// asserting the resident material's block is the authored value.

#include <array>
#include <cstring>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Task/TaskSystem.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 Size = 4;

    // Drains the worker pool and pumps the main-thread continuation so a pending
    // CreateAsync handle finalizes.
    void PumpToResidency(TaskSystem& tasks)
    {
        tasks.WaitForAll();
        tasks.PumpMainThread();
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "Texture::CreateAsync: a runtime texture streams in finalized and resident")
{
    // The async upload records on the transfer queue, so the per-worker transfer
    // pools the Application normally wires up must be initialized here.
    Context.InitializeTransferPools(Tasks);

    AssetManager assets(Context, Tasks, Types);

    std::array<u8, static_cast<usize>(Size) * Size * 4> pixels{};
    for (usize i = 0; i < static_cast<usize>(Size) * Size; i++)
    {
        pixels[i * 4 + 0] = 255;
        pixels[i * 4 + 3] = 255;
    }

    const TextureInfo info{
        .Name = "Async Build Texture",
        .Extent = {Size, Size},
        .Format = Format::RGBA8Unorm,
        .Pixels = pixels,
        .Sampler =
            {
                .AddressModeU = AddressMode::ClampToEdge,
                .AddressModeV = AddressMode::ClampToEdge,
                .AddressModeW = AddressMode::ClampToEdge,
            },
    };

    const AssetHandle<Texture> handle =
        assets.CreateAsync<Texture>(Texture::CreateAsync(Context, Tasks, info));

    // Pending: the handle exists but is not yet resident, and carries the invalid id.
    CHECK_FALSE(handle.IsLoaded());
    CHECK_FALSE(handle.Id().IsValid());

    PumpToResidency(Tasks);

    REQUIRE(handle.IsLoaded());
    const Texture* texture = handle.Get();
    REQUIRE(texture != nullptr);

    CHECK(texture->GetExtent() == uvec2(Size, Size));
    CHECK(texture->GetFormat() == Format::RGBA8Unorm);

    // The worker finalized it, so its bindless view/sampler slots are allocated.
    CHECK(texture->GetHandle().IsValid());
    CHECK(texture->GetSamplerHandle().IsValid());
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "Material::CreateAsync: a runtime material streams in with its authored block")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());

    const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F45});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    // The fragment shader pushes the material selector at offset 0 and reads a
    // float4 at byte 0 of the block, so the runtime material is a PostProcess
    // material (selector at 0) with a single float4 param.
    const Ref<PipelineLayout> layout = PipelineLayout::Create(
        Context, {
                     .Name = "Async Material Layout",
                     .PushConstantRanges =
                         {
                             PushConstantRange::Of<u32>(ShaderStage::Fragment),
                         },
                 });

    auto pipeline = GraphicsPipeline::Create(
        Context,
        {
            .Name = "Async Material Pipeline",
            .ColorAttachments = {{.Format = Format::RGBA8Unorm}},
            .PipelineLayout = layout,
            .ShaderStages =
                {
                    {.Stage = ShaderStage::Vertex, .Module = vertexAsset->Get()->Module},
                    {.Stage = ShaderStage::Fragment, .Module = fragmentAsset->Get()->Module},
                },
        });

    const vec4 authored{0.0f, 0.5f, 0.0f, 1.0f};
    vector<std::byte> block(sizeof(vec4));
    std::memcpy(block.data(), &authored, sizeof(authored));

    MaterialInfo info{
        .Name = "Async Build Material",
        .Context = &Context,
        .Domain = MaterialDomain::PostProcess,
        .Pipeline = pipeline,
        .VertexShader = *vertexAsset,
        .FragmentShader = *fragmentAsset,
        .Block = std::move(block),
        .Fields = {MaterialField{.Name = "Color",
                                 .Offset = 0,
                                 .Size = sizeof(vec4),
                                 .Kind = MaterialField::FieldKind::Param}},
        .SelectorOffset = 0,
    };

    const AssetHandle<Material> handle =
        assets.CreateAsync<Material>(Material::CreateAsync(Tasks, std::move(info), layout));

    CHECK_FALSE(handle.IsLoaded());
    CHECK_FALSE(handle.Id().IsValid());

    PumpToResidency(Tasks);

    REQUIRE(handle.IsLoaded());
    const Material* material = handle.Get();
    REQUIRE(material != nullptr);
    CHECK(material->GetDomain() == MaterialDomain::PostProcess);

    auto outputImage =
        Image::Create(Context, {
                                   .Name = "Async Material Output",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    auto outputView =
        ImageView::Create(Context, {.Name = "Async Material Output View", .Image = outputImage});

    auto& bindless = Context.GetBindlessRegistry();

    CommandBuffer& cmd = Context.BeginFrame();

    RenderGraph graph(Context);
    const ResourceId outputId = graph.Import("Output");
    graph.AddPass("Draw Async Material")
        .Color({
            .Resource = outputId,
            .Load = LoadOp::Clear,
            .Store = StoreOp::Store,
            .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
        })
        .Execute(
            [&](PassContext& ctx)
            {
                CommandBuffer& passCmd = ctx.Cmd();
                passCmd.SetViewport({0, 0}, {Size, Size});
                passCmd.SetScissor({0, 0}, {Size, Size});
                // Bind() binds the material's own pipeline and pushes the frame-folded
                // selector at offset 0 — exactly what the material_param shader reads.
                // It must precede bindless.Bind, which binds set 0 against the bound layout.
                material->Bind(passCmd);
                bindless.Bind(passCmd);
                passCmd.DrawFullscreenTriangle();
            });

    const RenderGraph::ImportBinding bindings[] = {{.Id = outputId, .View = outputView}};
    graph.Compile()->Execute(cmd, bindings);

    Context.EndFrame();
    Context.WaitIdle();

    const vector<u8> readback = outputImage->Download();
    REQUIRE(readback.size() == static_cast<usize>(Size) * Size * 4);

    // The fragment shader output the block's float4; the green channel round-trips
    // through RGBA8Unorm to ~0.5, proving the resident material's block is authored.
    const i32 want = static_cast<i32>(authored.y * 255.0f + 0.5f);
    CHECK(std::abs(static_cast<i32>(readback[1]) - want) <= 1);
}
