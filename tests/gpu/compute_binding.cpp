// Compute reads through every shape of binding a backend translates: a set holding a combined image
// sampler, a uniform buffer and a storage buffer; a dynamic uniform buffer at a bound offset; two
// halves of a push-constant block pushed apart, and a partial push that leaves the other half; and
// the bindless registry's texture, sampler and storage-buffer arrays, indexed by handle. Each read
// lands in its own output element, so a misplaced binding reads another binding's value or none.

#include <array>
#include <bit>
#include <cstring>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr AssetId BindingModelCompId{0xF5BBFEE534FDAFADULL};
    constexpr AssetId BindlessComputeCompId{0xC1E1ECDAC2BB39DEULL};

    // A texel whose channels differ, so a swizzled or wrong-format read shows.
    constexpr std::array<u8, 4> Texel{64, 128, 192, 255};

    vector<vec4> DownloadVec4s(const Ref<Buffer>& buffer)
    {
        const vector<u8> bytes = buffer->Download();
        vector<vec4> out(bytes.size() / sizeof(vec4));
        std::memcpy(out.data(), bytes.data(), out.size() * sizeof(vec4));
        return out;
    }

    Ref<ImageView> MakeTexelImage(Context& context)
    {
        const Ref<Image> image =
            Image::Create(context, {
                                       .Name = "Binding Texel",
                                       .Extent = {1, 1, 1},
                                       .Format = Format::RGBA8Unorm,
                                       .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                                   });
        image->UploadSync(Texel);
        return ImageView::Create(context, {.Name = "Binding Texel View", .Image = image});
    }

    bool NearTexel(const vec4 value)
    {
        const vec4 expected = vec4(Texel[0], Texel[1], Texel[2], Texel[3]) / 255.0f;
        return glm::all(glm::lessThan(glm::abs(value - expected), vec4(1e-3f)));
    }

    Ref<Buffer> MakeOutput(Context& context, const u32 count)
    {
        return Buffer::Create(context, {.Name = "Binding Output",
                                        .Size = count * sizeof(vec4),
                                        .Usage = BufferUsage::Storage | BufferUsage::TransferSrc});
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "compute binding: each binding shape reads its own value, and a partial push "
                  "keeps the other range")
{
    AssetManager assets(Context, Tasks, Types);
    const VoidResult mounted = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE_MESSAGE(mounted, mounted.error());
    const auto shader = assets.LoadSync<Shader>(BindingModelCompId);
    REQUIRE_MESSAGE(shader, shader.error().Detail);

    const Ref<ImageView> view = MakeTexelImage(Context);
    const Ref<Sampler> sampler = Sampler::Create(Context, {.Name = "Binding Sampler",
                                                           .MagFilter = Filter::Nearest,
                                                           .MinFilter = Filter::Nearest,
                                                           .MipmapMode = MipmapMode::Nearest});

    const vec4 params{1.0f, 2.0f, 3.0f, 4.0f};
    const Ref<Buffer> paramsBuffer = Buffer::Create(
        Context, {.Name = "Binding Params", .Size = sizeof(vec4), .Usage = BufferUsage::Uniform});
    paramsBuffer->UploadSync(std::span(reinterpret_cast<const u8*>(&params), sizeof(params)));

    // The dynamic block sits past a decoy, so a bound offset that is ignored reads the decoy.
    constexpr u32 DynamicOffset = 256;
    std::array<vec4, 32> dynamicBytes{};
    dynamicBytes[0] = vec4(-1.0f);
    dynamicBytes[DynamicOffset / sizeof(vec4)] = vec4(5.0f, 6.0f, 7.0f, 8.0f);
    const Ref<Buffer> dynamicBuffer = Buffer::Create(
        Context,
        {.Name = "Binding Dynamic", .Size = sizeof(dynamicBytes), .Usage = BufferUsage::Uniform});
    dynamicBuffer->UploadSync(
        std::span(reinterpret_cast<const u8*>(dynamicBytes.data()), sizeof(dynamicBytes)));

    const Ref<Buffer> output = MakeOutput(Context, 8);

    const Ref<DescriptorSetLayout> setLayout = DescriptorSetLayout::Create(
        Context,
        {.Name = "Binding Model Set",
         .Bindings = {
             {.Binding = 0,
              .Type = DescriptorType::CombinedImageSampler,
              .Stages = ShaderStage::Compute},
             {.Binding = 1, .Type = DescriptorType::UniformBuffer, .Stages = ShaderStage::Compute},
             {.Binding = 2, .Type = DescriptorType::StorageBuffer, .Stages = ShaderStage::Compute},
             {.Binding = 3,
              .Type = DescriptorType::UniformBufferDynamic,
              .Stages = ShaderStage::Compute},
         }});
    const Ref<DescriptorSet> set =
        DescriptorSet::Create(Context, {.Name = "Binding Model Set", .Layout = setLayout});
    set->Write(0, view, sampler);
    set->Write(1, paramsBuffer);
    set->Write(2, output);
    set->Write(3, dynamicBuffer, 0, sizeof(vec4));

    const Ref<PipelineLayout> layout = PipelineLayout::Create(
        Context, {.Name = "Binding Model Layout",
                  .DescriptorSetLayouts = {setLayout},
                  .PushConstantRanges = {
                      {.Stages = ShaderStage::Compute, .Offset = 0, .Size = 2 * sizeof(vec4)}}});
    const Ref<ComputePipeline> pipeline = ComputePipeline::Create(
        Context, {.Name = "Binding Model",
                  .PipelineLayout = layout,
                  .ShaderStage = {.Stage = ShaderStage::Compute, .Module = (*shader)->Module}});

    const vec4 first{9.0f, 10.0f, 11.0f, 12.0f};
    const vec4 second{13.0f, 14.0f, 15.0f, 16.0f};
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            cmd.BindPipeline(pipeline);
            cmd.BindDescriptorSets({set.get()}, 3, PipelineBindPoint::Compute, {DynamicOffset});
            cmd.PushConstants(first, 0);
            cmd.PushConstants(second, 16);
            cmd.Dispatch(1, 1, 1);
        });

    const vector<vec4> values = DownloadVec4s(output);
    REQUIRE(values.size() == 8);
    CHECK(NearTexel(values[0]));
    CHECK(values[1] == params);
    CHECK(values[2] == vec4(5.0f, 6.0f, 7.0f, 8.0f));
    CHECK(values[3] == first);
    CHECK(values[4] == second);

    // A second dispatch that pushes only the second range reads the first as last pushed.
    const vec4 replaced{17.0f, 18.0f, 19.0f, 20.0f};
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            cmd.BindPipeline(pipeline);
            cmd.BindDescriptorSets({set.get()}, 3, PipelineBindPoint::Compute, {DynamicOffset});
            cmd.PushConstants(first, 0);
            cmd.Dispatch(1, 1, 1);
            cmd.PrepareForAccess(output, {AccessKind::StorageBufferWrite},
                                 {AccessKind::StorageBufferWrite});
            cmd.PushConstants(replaced, 16);
            cmd.Dispatch(1, 1, 1);
        });

    const vector<vec4> partial = DownloadVec4s(output);
    CHECK(partial[3] == first);
    CHECK(partial[4] == replaced);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "compute binding: the bindless arrays read the resources their handles name")
{
    AssetManager assets(Context, Tasks, Types);
    const VoidResult mounted = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE_MESSAGE(mounted, mounted.error());
    const auto shader = assets.LoadSync<Shader>(BindlessComputeCompId);
    REQUIRE_MESSAGE(shader, shader.error().Detail);

    BindlessRegistry& bindless = Context.GetBindlessRegistry();

    // Registered after a decoy each, so a handle that indexes the wrong slot reads the decoy.
    const Ref<ImageView> decoyView = MakeTexelImage(Context);
    const TextureHandle decoy = bindless.Register(decoyView);
    const Ref<ImageView> view = MakeTexelImage(Context);
    const TextureHandle texture = bindless.Register(view);
    const SharedSampler sampler = bindless.AcquireSampler({.Name = "Bindless Compute Sampler",
                                                           .MagFilter = Filter::Nearest,
                                                           .MinFilter = Filter::Nearest});

    const std::array<f32, 8> words{0.0f, 0.0f, 0.0f, 0.0f, 21.0f, 22.0f, 23.0f, 24.0f};
    const Ref<Buffer> decoyBuffer = Buffer::Create(
        Context, {.Name = "Bindless Decoy", .Size = sizeof(words), .Usage = BufferUsage::Storage});
    const StorageBufferHandle decoyHandle = bindless.Register(decoyBuffer);
    const Ref<Buffer> wordBuffer = Buffer::Create(
        Context, {.Name = "Bindless Words", .Size = sizeof(words), .Usage = BufferUsage::Storage});
    wordBuffer->UploadSync(std::span(reinterpret_cast<const u8*>(words.data()), sizeof(words)));
    const StorageBufferHandle wordHandle = bindless.Register(wordBuffer);

    const Ref<Buffer> output = MakeOutput(Context, 2);
    const Ref<DescriptorSetLayout> setLayout =
        DescriptorSetLayout::Create(Context, {.Name = "Bindless Compute Set",
                                              .Bindings = {{.Binding = 0,
                                                            .Type = DescriptorType::StorageBuffer,
                                                            .Stages = ShaderStage::Compute}}});
    const Ref<DescriptorSet> set =
        DescriptorSet::Create(Context, {.Name = "Bindless Compute Set", .Layout = setLayout});
    set->Write(0, output);

    struct Push
    {
        u32 Texture;
        u32 Sampler;
        u32 Buffer;
        u32 Pad;
    };
    const Ref<PipelineLayout> layout = PipelineLayout::Create(
        Context, {.Name = "Bindless Compute Layout",
                  .DescriptorSetLayouts = {setLayout},
                  .PushConstantRanges = {PushConstantRange::Of<Push>(ShaderStage::Compute)}});
    const Ref<ComputePipeline> pipeline = ComputePipeline::Create(
        Context, {.Name = "Bindless Compute",
                  .PipelineLayout = layout,
                  .ShaderStage = {.Stage = ShaderStage::Compute, .Module = (*shader)->Module}});

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            cmd.BindPipeline(pipeline);
            bindless.Bind(cmd, PipelineBindPoint::Compute);
            cmd.BindDescriptorSets({set.get()}, 3, PipelineBindPoint::Compute);
            cmd.PushConstants(Push{.Texture = texture.Index,
                                   .Sampler = sampler.Handle.Index,
                                   .Buffer = wordHandle.Index});
            cmd.Dispatch(1, 1, 1);
        });

    const vector<vec4> values = DownloadVec4s(output);
    REQUIRE(values.size() == 2);
    CHECK(NearTexel(values[0]));
    CHECK(values[1] == vec4(21.0f, 22.0f, 23.0f, 24.0f));

    bindless.Release(texture);
    bindless.Release(decoy);
    bindless.Release(wordHandle);
    bindless.Release(decoyHandle);
}
