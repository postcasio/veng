// The byte-address storage-buffer bindless input (set-0 g_Buffers[]) — the plan's
// load-bearing descriptor-layout spike, plus the SetStorageBufferHandle material path.
//
//   1. Descriptor-layout spike: register a byte-address storage buffer into set 0's
//      g_Buffers[] array beside the sampler array, then read a known float4 from it in
//      a fragment shader that also samples a bindless texture. Proves a non-dynamic,
//      full-range byte-address buffer array coexists with a sampler in one descriptor
//      set (the MoltenVK argument-buffer question) — the output pixel reflects the
//      buffer's contents. Runs under the validation gate, so an unallowlisted
//      validation ERROR from the new binding fails the run.
//   2. Register/Release cycling: a released storage-buffer slot is not reused until it
//      has cycled through every frame-in-flight, mirroring the image/sampler contract.

#include <array>
#include <cstring>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 Size = 4;

    struct BufferPushConstants
    {
        u32 BufferIndex;
        u32 TextureIndex;
        u32 SamplerIndex;
    };
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "bindless storage buffer: register a byte-address buffer in set 0 beside a "
                  "sampler and read it from a fragment shader")
{
    // The known value the buffer holds and the fragment writes back: opaque blue.
    // Exactly representable in RGBA8Unorm.
    constexpr std::array<u8, 4> expected = {0, 0, 255, 255};
    const std::array<f32, 4> color = {0.0f, 0.0f, 1.0f, 1.0f};

    // A host-mapped storage buffer seeded with the float4, registered into the bindless
    // g_Buffers[] array. Bound at full range, read by handle index — no dynamic offset.
    auto buffer = Buffer::Create(Context, {
                                              .Name = "Bindless Storage Buffer",
                                              .Size = sizeof(color),
                                              .Usage = BufferUsage::Storage,
                                              .HostMapped = true,
                                          });
    std::memcpy(buffer->GetMappedData(), color.data(), sizeof(color));

    // A paired texture/sampler so the fragment exercises the sampler binding alongside
    // the buffer array in the same set-0 descriptor set (the coexistence check).
    auto sourceImage = Image::Create(Context, {
                                                  .Name = "Buffer Spike Source",
                                                  .Extent = {Size, Size, 1},
                                                  .Format = Format::RGBA8Unorm,
                                                  .Usage = ImageUsage::Sampled,
                                              });
    auto sourceView =
        ImageView::Create(Context, {.Name = "Buffer Spike Source View", .Image = sourceImage});
    auto sampler = Sampler::Create(Context, {
                                                .Name = "Buffer Spike Sampler",
                                                .AddressModeU = AddressMode::ClampToEdge,
                                                .AddressModeV = AddressMode::ClampToEdge,
                                                .AddressModeW = AddressMode::ClampToEdge,
                                            });

    auto outputImage =
        Image::Create(Context, {
                                   .Name = "Buffer Spike Output",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    auto outputView =
        ImageView::Create(Context, {.Name = "Buffer Spike Output View", .Image = outputImage});

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE(mountResult.has_value());

    const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F4C});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    const Ref<PipelineLayout> layout = PipelineLayout::Create(
        Context, {
                     .Name = "Buffer Spike Layout",
                     .PushConstantRanges =
                         {
                             PushConstantRange::Of<BufferPushConstants>(ShaderStage::Fragment),
                         },
                 });
    auto pipeline = GraphicsPipeline::Create(
        Context,
        {
            .Name = "Buffer Spike Pipeline",
            .ColorAttachments = {{.Format = Format::RGBA8Unorm}},
            .PipelineLayout = layout,
            .ShaderStages =
                {
                    {.Stage = ShaderStage::Vertex, .Module = vertexAsset->Get()->Module},
                    {.Stage = ShaderStage::Fragment, .Module = fragmentAsset->Get()->Module},
                },
        });

    auto& bindless = Context.GetBindlessRegistry();
    const StorageBufferHandle bufferHandle = bindless.Register(buffer);
    const TextureHandle textureHandle = bindless.Register(sourceView);
    const SamplerHandle samplerHandle = bindless.Register(sampler);

    CHECK(bufferHandle.IsValid());

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId outputId = graph.Import("Output");

            graph.AddPass("Read Storage Buffer")
                .Color({
                    .Resource = outputId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                })
                .Execute(
                    [&](PassContext& ctx)
                    {
                        CommandBuffer& inner = ctx.Cmd();
                        inner.BindPipeline(pipeline);
                        inner.SetViewport({0, 0}, {Size, Size});
                        inner.SetScissor({0, 0}, {Size, Size});
                        bindless.Bind(inner);
                        inner.PushConstants(BufferPushConstants{
                            .BufferIndex = bufferHandle.Index,
                            .TextureIndex = textureHandle.Index,
                            .SamplerIndex = samplerHandle.Index,
                        });
                        inner.DrawFullscreenTriangle();
                    });

            const RenderGraph::ImportBinding bindings[] = {
                {.Id = outputId, .View = outputView},
            };
            graph.Compile()->Execute(cmd, bindings);
        });

    const vector<u8> pixels = outputImage->Download();
    REQUIRE(pixels.size() == static_cast<size_t>(Size) * Size * 4);
    CHECK(Test::PixelsMatch(pixels, expected));

    bindless.Release(bufferHandle);
    bindless.Release(textureHandle);
    bindless.Release(samplerHandle);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "bindless storage buffer: a material reaches a storage buffer by a handle in its "
                  "param block")
{
    // The material-input two-hop: a material's param block carries a g_Buffers[] handle (what
    // MaterialInstance::SetStorageBufferHandle writes), and the shader loads the buffer's contents
    // through it. Proves the storage-buffer material input end to end at the block level.
    constexpr std::array<u8, 4> expected = {0, 255, 0, 255};
    const std::array<f32, 4> color = {0.0f, 1.0f, 0.0f, 1.0f};

    auto buffer = Buffer::Create(Context, {
                                              .Name = "Material Buffer Input",
                                              .Size = sizeof(color),
                                              .Usage = BufferUsage::Storage,
                                              .HostMapped = true,
                                          });
    std::memcpy(buffer->GetMappedData(), color.data(), sizeof(color));

    auto outputImage =
        Image::Create(Context, {
                                   .Name = "Material Buffer Output",
                                   .Extent = {Size, Size, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    auto outputView =
        ImageView::Create(Context, {.Name = "Material Buffer Output View", .Image = outputImage});

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());
    const AssetResult<AssetHandle<Shader>> vertexAsset = assets.LoadSync<Shader>(AssetId{0x1F42});
    const AssetResult<AssetHandle<Shader>> fragmentAsset = assets.LoadSync<Shader>(AssetId{0x1F4D});
    REQUIRE(vertexAsset.has_value());
    REQUIRE(fragmentAsset.has_value());

    struct MaterialPush
    {
        u32 MaterialIndex;
    };
    const Ref<PipelineLayout> layout = PipelineLayout::Create(
        Context,
        {
            .Name = "Material Buffer Layout",
            .PushConstantRanges = {PushConstantRange::Of<MaterialPush>(ShaderStage::Fragment)},
        });
    auto pipeline = GraphicsPipeline::Create(
        Context,
        {
            .Name = "Material Buffer Pipeline",
            .ColorAttachments = {{.Format = Format::RGBA8Unorm}},
            .PipelineLayout = layout,
            .ShaderStages =
                {
                    {.Stage = ShaderStage::Vertex, .Module = vertexAsset->Get()->Module},
                    {.Stage = ShaderStage::Fragment, .Module = fragmentAsset->Get()->Module},
                },
        });

    auto& bindless = Context.GetBindlessRegistry();
    const StorageBufferHandle bufferHandle = bindless.Register(buffer);

    // A material block whose first uint is the buffer handle — exactly what
    // SetStorageBufferHandle writes at a StorageBufferHandle field's offset.
    std::array<u8, 4> block{};
    std::memcpy(block.data(), &bufferHandle.Index, sizeof(u32));
    const MaterialHandle materialHandle = bindless.RegisterMaterial(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(block.data()), block.size()));

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId outputId = graph.Import("Output");
            graph.AddPass("Read Material Buffer")
                .Color({
                    .Resource = outputId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                })
                .Execute(
                    [&](PassContext& ctx)
                    {
                        CommandBuffer& inner = ctx.Cmd();
                        inner.BindPipeline(pipeline);
                        inner.SetViewport({0, 0}, {Size, Size});
                        inner.SetScissor({0, 0}, {Size, Size});
                        bindless.Bind(inner);
                        // Fold in the current frame's material region base, as a draw does.
                        inner.PushConstants(MaterialPush{
                            .MaterialIndex = bindless.GetCurrentFrameBase() + materialHandle.Index,
                        });
                        inner.DrawFullscreenTriangle();
                    });

            const RenderGraph::ImportBinding bindings[] = {{.Id = outputId, .View = outputView}};
            graph.Compile()->Execute(cmd, bindings);
        });

    const vector<u8> pixels = outputImage->Download();
    REQUIRE(pixels.size() == static_cast<size_t>(Size) * Size * 4);
    CHECK(Test::PixelsMatch(pixels, expected));

    bindless.Release(materialHandle);
    bindless.Release(bufferHandle);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "bindless storage buffer: released slots are not reused "
                                          "until they cycle through every frame in flight")
{
    auto& bindless = Context.GetBindlessRegistry();

    auto MakeBuffer = [&](string_view name)
    {
        return Buffer::Create(Context, {
                                           .Name = string(name),
                                           .Size = 16,
                                           .Usage = BufferUsage::Storage,
                                           .HostMapped = true,
                                       });
    };

    auto bufA = MakeBuffer("Buf A");
    auto bufB = MakeBuffer("Buf B");
    auto bufC = MakeBuffer("Buf C");
    auto bufD = MakeBuffer("Buf D");

    const StorageBufferHandle handleA = bindless.Register(bufA);
    const StorageBufferHandle handleB = bindless.Register(bufB);

    bindless.Release(handleA);

    // The freed slot must not be handed back immediately — an in-flight frame may still
    // reference it.
    const StorageBufferHandle handleC = bindless.Register(bufC);
    CHECK(handleC.Index != handleA.Index);

    const u32 framesInFlight = Context.GetMaxFramesInFlight();
    for (u32 i = 0; i < framesInFlight + 1; i++)
    {
        Context.BeginFrame();
        Context.EndFrame();
    }

    const StorageBufferHandle handleD = bindless.Register(bufD);
    CHECK(handleD.Index == handleA.Index);

    bindless.Release(handleB);
    bindless.Release(handleC);
    bindless.Release(handleD);
}
