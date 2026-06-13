// Descriptor write paths test (planset-3, plan 06): exercises the
// DescriptorSet::Write overloads for each DescriptorType without a full
// render — allocate a layout + set per binding type and write a resource into
// it, asserting the call itself succeeds (VE_ASSERT would abort otherwise).
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
//
// Skips cleanly (exit 77, ctest reports it as skipped) on a machine with no
// usable Vulkan ICD, via Test::HasVulkanDriver() (planset-3, plan 01/06).

#include <cstdio>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

#include <support/GpuContext.h>
#include <support/GpuProbe.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // Allocates a single-binding layout + set of `type` and writes `write` into
    // binding 0. Returns the set so it stays alive for the caller's scope.
    template <typename Write>
    Ref<DescriptorSet> WriteSingleBinding(string_view name, DescriptorType type, ShaderStage stages, Write&& write)
    {
        auto layout = DescriptorSetLayout::Create({
            .Name = string(name) + " Layout",
            .Bindings = {{.Binding = 0, .Type = type, .Count = 1, .Stages = stages}},
        });

        auto set = DescriptorSet::Create({
            .Name = string(name) + " Set",
            .Layout = layout,
        });

        write(set);

        return set;
    }
}

int main()
{
    if (!Test::HasVulkanDriver())
        return 77;

    constexpr u32 size = 4;

    Test::GpuContext gpu("Descriptor Write Paths Test", {size, size});
    Context& context = gpu.Get();

    int status = 0;
    {
        auto sampledImage = Image::Create(context, {
            .Name = "Sampled Image",
            .Extent = {size, size, 1},
            .Format = Format::RGBA8Unorm,
            .Usage = ImageUsage::Sampled,
        });
        auto sampledView = ImageView::Create(context, {.Name = "Sampled Image View", .Image = sampledImage});

        auto storageImage = Image::Create(context, {
            .Name = "Storage Image",
            .Extent = {size, size, 1},
            .Format = Format::RGBA8Unorm,
            .Usage = ImageUsage::Storage,
        });
        auto storageView = ImageView::Create(context, {.Name = "Storage Image View", .Image = storageImage});

        auto sampler = Sampler::Create(context, {
            .Name = "Descriptor Test Sampler",
            .AddressModeU = AddressMode::ClampToEdge,
            .AddressModeV = AddressMode::ClampToEdge,
            .AddressModeW = AddressMode::ClampToEdge,
        });

        auto uniformBuffer = Buffer::Create(context, {
            .Name = "Uniform Buffer",
            .Size = 64,
            .Usage = BufferUsage::Uniform | BufferUsage::TransferDst,
        });

        auto storageBuffer = Buffer::Create(context, {
            .Name = "Storage Buffer",
            .Size = 256,
            .Usage = BufferUsage::Storage | BufferUsage::TransferDst,
        });

        // CombinedImageSampler
        auto combinedSet = WriteSingleBinding("Combined Image Sampler", DescriptorType::CombinedImageSampler,
            ShaderStage::Fragment, [&](const Ref<DescriptorSet>& set)
            {
                set->Write(0, sampledView, sampler);
            });

        // SampledImage
        auto sampledSet = WriteSingleBinding("Sampled Image", DescriptorType::SampledImage,
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
        auto storageImageSet = WriteSingleBinding("Storage Image", DescriptorType::StorageImage,
            ShaderStage::Compute, [&](const Ref<DescriptorSet>& set)
            {
                set->Write(0, storageView);
            });

        // UniformBuffer
        auto uniformSet = WriteSingleBinding("Uniform Buffer", DescriptorType::UniformBuffer,
            ShaderStage::Vertex | ShaderStage::Fragment, [&](const Ref<DescriptorSet>& set)
            {
                set->Write(0, uniformBuffer);
            });

        // StorageBuffer
        auto storageBufferSet = WriteSingleBinding("Storage Buffer", DescriptorType::StorageBuffer,
            ShaderStage::Compute, [&](const Ref<DescriptorSet>& set)
            {
                set->Write(0, storageBuffer);
            });

        // Reaching here without an abort means every Write overload succeeded.
        (void)combinedSet;
        (void)sampledSet;
        (void)storageImageSet;
        (void)uniformSet;
        (void)storageBufferSet;
    }

    if (status == 0)
    {
        std::printf("OK: descriptor write paths verified "
                     "(CombinedImageSampler, SampledImage, StorageImage, UniformBuffer, StorageBuffer)\n");
    }

    return status;
}
