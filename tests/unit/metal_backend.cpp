// The Metal backend's own device-free logic: the placement-heap sub-allocator, the binding model's
// element and slot assignment, the push-constant shadow and ring, and the mapping of a hazard
// decision onto Metal stages. Registered only in a Metal tree, beside the backend it tests.

#include <array>
#include <cstring>

#include <doctest/doctest.h>

#include <Renderer/Backend/Metal/BarrierMapping.h>
#include <Renderer/Backend/Metal/BindingModel.h>
#include <Renderer/Backend/Metal/HeapAllocator.h>
#include <Renderer/Backend/Metal/PushConstantRing.h>
#include <Veng/Renderer/BindlessRegistry.h>

using namespace Veng;
using namespace Veng::Renderer;
using namespace Veng::Renderer::Metal;

TEST_CASE("Metal heap: placements are aligned, disjoint and lowest first")
{
    HeapBlockAllocator heap(4096);
    const optional<u64> a = heap.Allocate(100, 64);
    const optional<u64> b = heap.Allocate(100, 256);
    const optional<u64> c = heap.Allocate(10, 1);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());
    CHECK(*a == 0);
    CHECK(*b % 256 == 0);
    CHECK(*b >= *a + 100);
    // The gap the second placement's alignment left is filled first.
    CHECK(*c == 100);
    CHECK(heap.GetUsed() == 210);
    CHECK_FALSE(heap.Allocate(4096, 1).has_value());
}

TEST_CASE("Metal heap: a freed range is reused and neighbours coalesce")
{
    HeapBlockAllocator heap(1024);
    const u64 a = *heap.Allocate(256, 256);
    const u64 b = *heap.Allocate(256, 256);
    const u64 c = *heap.Allocate(256, 256);
    CHECK_FALSE(heap.Allocate(512, 256).has_value());

    heap.Free(b, 256);
    CHECK(*heap.Allocate(256, 256) == b);
    heap.Free(b, 256);

    // Freeing both neighbours of the hole leaves one free run from 0 to the end.
    heap.Free(a, 256);
    heap.Free(c, 256);
    CHECK(heap.IsEmpty());
    CHECK(heap.GetFreeRangeCount() == 1);
    CHECK(*heap.Allocate(1024, 1024) == 0);
}

TEST_CASE("Metal heap: a large placement takes a heap of its own")
{
    CHECK_FALSE(IsDedicatedPlacement(HeapBlockBytes / 4));
    CHECK(IsDedicatedPlacement(HeapBlockBytes / 4 + 1));
    CHECK(HeapBytesFor(1) == HeapBlockBytes);
    const u64 large = HeapBlockBytes + 1;
    CHECK(HeapBytesFor(large) >= large);
    CHECK(HeapBytesFor(large) % HeapPageBytes == 0);
}

TEST_CASE("Metal binding model: the bindless set places each array where the cook does")
{
    // The elements the cook records for set 0, in binding order.
    const vector<DescriptorBinding> bindings{
        {.Binding = BindlessRegistry::TextureBinding,
         .Type = DescriptorType::SampledImage,
         .Count = BindlessRegistry::MaxTextures},
        {.Binding = BindlessRegistry::SamplerBinding,
         .Type = DescriptorType::Sampler,
         .Count = BindlessRegistry::MaxSamplers},
        {.Binding = BindlessRegistry::StorageImageBinding,
         .Type = DescriptorType::StorageImage,
         .Count = BindlessRegistry::MaxStorageImages},
        {.Binding = BindlessRegistry::StorageBufferBinding,
         .Type = DescriptorType::StorageBuffer,
         .Count = BindlessRegistry::MaxStorageBuffers},
        {.Binding = BindlessRegistry::MaterialParamBinding, .Type = DescriptorType::StorageBuffer},
        {.Binding = BindlessRegistry::ViewConstantsBinding, .Type = DescriptorType::StorageBuffer},
        {.Binding = BindlessRegistry::LightBinding, .Type = DescriptorType::StorageBuffer},
        {.Binding = BindlessRegistry::AreaVertexBinding, .Type = DescriptorType::StorageBuffer},
        {.Binding = BindlessRegistry::NormalVarianceBinding,
         .Type = DescriptorType::SampledImage,
         .Count = BindlessRegistry::MaxTextures},
    };
    const SetPlacement placement = PlaceSet("set 0", bindings);
    const std::array<std::pair<u32, u32>, 9> expected{{
        {BindlessRegistry::TextureBinding, 0},
        {BindlessRegistry::SamplerBinding, 1024},
        {BindlessRegistry::StorageImageBinding, 1152},
        {BindlessRegistry::StorageBufferBinding, 1664},
        {BindlessRegistry::MaterialParamBinding, 1920},
        {BindlessRegistry::ViewConstantsBinding, 1921},
        {BindlessRegistry::LightBinding, 1922},
        {BindlessRegistry::AreaVertexBinding, 1923},
        {BindlessRegistry::NormalVarianceBinding, 1924},
    }};
    for (const auto& [binding, element] : expected)
    {
        CAPTURE(binding);
        REQUIRE(placement.Find(binding) != nullptr);
        CHECK(placement.Find(binding)->FirstElement == element);
    }
    CHECK(placement.ElementCount == 1924 + BindlessRegistry::MaxTextures);
    CHECK(placement.DynamicCount == 0);
}

TEST_CASE("Metal binding model: combined samplers take two elements, arrays one per entry, and "
          "dynamic buffers none")
{
    // Declared out of order: placement follows the binding numbers, not the list.
    const vector<DescriptorBinding> bindings{
        {.Binding = 3, .Type = DescriptorType::UniformBufferDynamic},
        {.Binding = 1, .Type = DescriptorType::CombinedImageSampler, .Count = 3},
        {.Binding = 0, .Type = DescriptorType::CombinedImageSampler},
        {.Binding = 2, .Type = DescriptorType::StorageImage},
        {.Binding = 4, .Type = DescriptorType::UniformBufferDynamic},
    };
    const SetPlacement placement = PlaceSet("mixed", bindings);
    CHECK(placement.Find(0)->FirstElement == 0);
    CHECK(placement.Find(1)->FirstElement == 2);
    CHECK(placement.Find(2)->FirstElement == 8);
    CHECK(placement.ElementCount == 9);
    CHECK(placement.DynamicCount == 2);
    CHECK(placement.Find(3)->DynamicIndex == 0);
    CHECK(placement.Find(4)->DynamicIndex == 1);
    CHECK(placement.Find(5) == nullptr);
}

TEST_CASE("Metal binding model: dynamic buffers take slots in set then binding order")
{
    namespace Slots = Cook::MetalBindingSlots;
    // The three bindless sets carry none; set 3 two, set 4 none, set 5 one.
    const std::array<u32, 6> counts{0, 0, 0, 2, 0, 1};
    const vector<u32> slots = DynamicBufferSlots("layout", counts);
    REQUIRE(slots.size() == counts.size());
    CHECK(slots[3] == Slots::DynamicUniformBufferBase);
    CHECK(slots[5] == Slots::DynamicUniformBufferBase + 2);
    CHECK(slots[5] < Slots::PushConstants);
}

TEST_CASE("Metal push constants: partial and per-stage writes land at their offsets")
{
    PushConstantShadow shadow;
    shadow.SetSize(32);
    CHECK(shadow.IsDirty());
    shadow.MarkClean();

    const std::array<u32, 4> vertex{1, 2, 3, 4};
    const std::array<u32, 4> fragment{5, 6, 7, 8};
    shadow.Write(0, std::as_bytes(std::span(vertex)));
    CHECK(shadow.IsDirty());
    shadow.MarkClean();
    shadow.Write(16, std::as_bytes(std::span(fragment)));

    std::array<u32, 8> block{};
    std::memcpy(block.data(), shadow.GetBytes().data(), sizeof(block));
    CHECK(block == std::array<u32, 8>{1, 2, 3, 4, 5, 6, 7, 8});

    // A partial write keeps the rest of the block.
    const u32 replaced = 9;
    shadow.Write(4, std::as_bytes(std::span(&replaced, 1)));
    std::memcpy(block.data(), shadow.GetBytes().data(), sizeof(block));
    CHECK(block == std::array<u32, 8>{1, 9, 3, 4, 5, 6, 7, 8});

    // The same size again is no change; a new size is.
    shadow.MarkClean();
    shadow.SetSize(32);
    CHECK_FALSE(shadow.IsDirty());
    shadow.SetSize(64);
    CHECK(shadow.IsDirty());
}

TEST_CASE("Metal push constants: the ring places copies aligned, spills to a new chunk, and "
          "rewinds")
{
    LinearRing ring;
    const RingPlacement first = ring.Allocate(32);
    const RingPlacement second = ring.Allocate(16);
    CHECK(first.Chunk == 0);
    CHECK(first.Offset == 0);
    CHECK(second.Offset == PushConstantCopyAlignment);

    u32 lastChunk = 0;
    u32 lastOffset = second.Offset;
    const u32 perChunk = PushConstantChunkBytes / PushConstantCopyAlignment;
    for (u32 i = 0; i < perChunk; ++i)
    {
        const RingPlacement placement = ring.Allocate(64);
        CHECK(placement.Offset % PushConstantCopyAlignment == 0);
        CHECK(placement.Offset + 64 <= PushConstantChunkBytes);
        lastChunk = placement.Chunk;
        lastOffset = placement.Offset;
    }
    CHECK(lastChunk == 1);
    CHECK(ring.GetChunksUsed() == 2);
    CHECK(lastOffset < PushConstantChunkBytes);

    // Rewinding, as a command buffer does when it is begun again, reuses the first chunk.
    ring.Rewind();
    const RingPlacement rewound = ring.Allocate(16);
    CHECK(rewound.Chunk == 0);
    CHECK(rewound.Offset == 0);
    CHECK(ring.GetChunksUsed() == 2);
}

TEST_CASE("Metal barriers: a hazard maps onto the stages that run it, and a read after a read "
          "orders nothing")
{
    const MetalBarrier computeRaw = MapBarrier(SyncStage::ComputeShader, SyncAccess::ShaderWrite,
                                               SyncStage::ComputeShader, SyncAccess::ShaderRead);
    CHECK(computeRaw.IsNeeded());
    CHECK(computeRaw.After == MetalStage::Dispatch);
    CHECK(computeRaw.Before == MetalStage::Dispatch);

    const MetalBarrier upload = MapBarrier(SyncStage::Transfer, SyncAccess::TransferWrite,
                                           SyncStage::FragmentShader, SyncAccess::ShaderRead);
    CHECK(HasFlag(upload.After, MetalStage::Blit));
    CHECK(upload.Before == MetalStage::Fragment);

    const MetalBarrier war = MapBarrier(SyncStage::FragmentShader, SyncAccess::ShaderRead,
                                        SyncStage::ComputeShader, SyncAccess::ShaderWrite);
    CHECK(war.IsNeeded());

    CHECK_FALSE(MapBarrier(SyncStage::FragmentShader, SyncAccess::ShaderRead,
                           SyncStage::ComputeShader, SyncAccess::ShaderRead)
                    .IsNeeded());

    // A first use waits on nothing, and the scopes the pipe's ends name follow Vulkan's reading.
    CHECK_FALSE(MapBarrier(SyncStage::TopOfPipe, SyncAccess::None, SyncStage::Transfer,
                           SyncAccess::TransferWrite)
                    .IsNeeded());
    CHECK(ToMetalStages(SyncStage::AllCommands, true) == MetalStage::All);
    CHECK(ToMetalStages(SyncStage::BottomOfPipe, true) == MetalStage::All);
    CHECK(ToMetalStages(SyncStage::BottomOfPipe, false) == MetalStage::None);
    CHECK(ToMetalStages(SyncStage::TopOfPipe, false) == MetalStage::All);
    CHECK(ToMetalStages(SyncStage::Host, true) == MetalStage::None);
    CHECK(HasFlag(ToMetalStages(SyncStage::DrawIndirect, false), MetalStage::Dispatch));
}
