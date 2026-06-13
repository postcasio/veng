// Descriptor write paths cases (ported from the planset-3 plan 06 one-exe
// test): exercises the DescriptorSet::Write overloads for each DescriptorType
// without a full render — allocate a layout + set per binding type and write
// a resource into it, asserting the call itself succeeds (VE_ASSERT would
// abort otherwise).
//
//   - CombinedImageSampler: Write(binding, view, sampler)
//   - SampledImage:         Write(binding, view)
//   - StorageImage:         Write(binding, view)
//   - UniformBuffer:        Write(binding, buffer)
//   - StorageBuffer:        Write(binding, buffer)
//
// Known validation gap (CLAUDE.md, "Known validation gap"): the storage-image
// descriptor path sets UPDATE_AFTER_BIND without enabling
// descriptorBindingStorageImageUpdateAfterBind, and the headless descriptor
// pool has no STORAGE_IMAGE pool size. Under VE_DEBUG this binding's
// allocate/write produces Vulkan validation messages on stderr — that is the
// documented gap, not a bug in this test, and it does not fail the write itself
// or this test at the ctest level (validation errors don't abort, per CLAUDE.md).
//
// NEW (discovered by this test, planset-3 plan 06): the headless descriptor
// pool also has no SAMPLED_IMAGE pool size, so a standalone SampledImage binding
// emits the same pool-size WARN under VE_DEBUG. Production code reaches sampled
// images via CombinedImageSampler (which the pool does cover), so this only
// surfaces with a bare SampledImage descriptor. Same gap family — recorded for
// the bindless/descriptor rework (plans/future/bindless-descriptors.md), not
// fixed here.

#include <doctest/doctest.h>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 kSize = 4;

    // Allocates a single-binding layout + set of `type` and writes `write` into
    // binding 0. Returns the set so it stays alive for the caller's scope.
    template <typename Write>
    Ref<DescriptorSet> WriteSingleBinding(Context& context, string_view name, DescriptorType type, ShaderStage stages, Write&& write)
    {
        auto layout = DescriptorSetLayout::Create(context, {
            .Name = string(name) + " Layout",
            .Bindings = {{.Binding = 0, .Type = type, .Count = 1, .Stages = stages}},
        });

        auto set = DescriptorSet::Create(context, {
            .Name = string(name) + " Set",
            .Layout = layout,
        });

        write(set);

        return set;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "descriptor write paths: all Write overloads succeed")
{
    auto sampledImage = Image::Create(Context, {
        .Name = "Sampled Image",
        .Extent = {kSize, kSize, 1},
        .Format = Format::RGBA8Unorm,
        .Usage = ImageUsage::Sampled,
    });
    auto sampledView = ImageView::Create(Context, {.Name = "Sampled Image View", .Image = sampledImage});

    auto storageImage = Image::Create(Context, {
        .Name = "Storage Image",
        .Extent = {kSize, kSize, 1},
        .Format = Format::RGBA8Unorm,
        .Usage = ImageUsage::Storage,
    });
    auto storageView = ImageView::Create(Context, {.Name = "Storage Image View", .Image = storageImage});

    auto sampler = Sampler::Create(Context, {
        .Name = "Descriptor Test Sampler",
        .AddressModeU = AddressMode::ClampToEdge,
        .AddressModeV = AddressMode::ClampToEdge,
        .AddressModeW = AddressMode::ClampToEdge,
    });

    auto uniformBuffer = Buffer::Create(Context, {
        .Name = "Uniform Buffer",
        .Size = 64,
        .Usage = BufferUsage::Uniform | BufferUsage::TransferDst,
    });

    auto storageBuffer = Buffer::Create(Context, {
        .Name = "Storage Buffer",
        .Size = 256,
        .Usage = BufferUsage::Storage | BufferUsage::TransferDst,
    });

    // CombinedImageSampler
    auto combinedSet = WriteSingleBinding(Context, "Combined Image Sampler", DescriptorType::CombinedImageSampler,
        ShaderStage::Fragment, [&](const Ref<DescriptorSet>& set)
        {
            set->Write(0, sampledView, sampler);
        });

    // SampledImage
    auto sampledSet = WriteSingleBinding(Context, "Sampled Image", DescriptorType::SampledImage,
        ShaderStage::Fragment, [&](const Ref<DescriptorSet>& set)
        {
            set->Write(0, sampledView);
        });

    // StorageImage — known validation gap (see header comment): the
    // headless descriptor pool has no STORAGE_IMAGE pool size and
    // UPDATE_AFTER_BIND is set without the matching device feature, so this
    // allocate/write may emit Vulkan validation messages under VE_DEBUG.
    // Validation errors don't abort (CLAUDE.md), so the call still
    // completes and this test still passes.
    auto storageImageSet = WriteSingleBinding(Context, "Storage Image", DescriptorType::StorageImage,
        ShaderStage::Compute, [&](const Ref<DescriptorSet>& set)
        {
            set->Write(0, storageView);
        });

    // UniformBuffer
    auto uniformSet = WriteSingleBinding(Context, "Uniform Buffer", DescriptorType::UniformBuffer,
        ShaderStage::Vertex | ShaderStage::Fragment, [&](const Ref<DescriptorSet>& set)
        {
            set->Write(0, uniformBuffer);
        });

    // StorageBuffer
    auto storageBufferSet = WriteSingleBinding(Context, "Storage Buffer", DescriptorType::StorageBuffer,
        ShaderStage::Compute, [&](const Ref<DescriptorSet>& set)
        {
            set->Write(0, storageBuffer);
        });

    // Reaching here without an abort means every Write overload succeeded.
    CHECK(combinedSet != nullptr);
    CHECK(sampledSet != nullptr);
    CHECK(storageImageSet != nullptr);
    CHECK(uniformSet != nullptr);
    CHECK(storageBufferSet != nullptr);
}
