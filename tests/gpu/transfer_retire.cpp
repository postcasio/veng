// The Context's deferred-destruction paths:
//
// 1. RetireOnTransfer keys a raw allocation on the transfer timeline, not a
//    frame fence: the buffer is destroyed only once the timeline reaches the
//    value it was retired against, and the drain runs at teardown even with no
//    intervening frame (the upload-then-immediate-dispose case).
// 2. ReleaseBuffer hands out the raw (vk::Buffer, VmaAllocation) and nulls the
//    wrapper so its destructor does NOT route the handle through the frame bin.
// 3. AddFrameTransferWait accumulates a (timeline, value) the next frame submit
//    folds in — exercised through a headless SubmitFrame.
// 4. The pending bin: a drop between frames waits for the next frame's fence, and
//    a drop outside any frame is released after an ImmediateCommands recording
//    once nothing else is in flight.
// 5. What else waits on a frame in a process that never begins one: reached
//    upload scratch, a queued bindless acquire, and the transfer waits an
//    out-of-frame ImmediateCommands submit has already taken.
//
// These deal in raw Vulkan/VMA handles by design, so this case reaches through
// the public Native.h escape hatch; veng_gpu links Vulkan + VMA for it.

#include <doctest/doctest.h>

#include <algorithm>
#include <mutex>

#include <Veng/Renderer/Backend/Barrier.h>
#include <Veng/Renderer/Backend/Natives.h>
#include <Veng/Renderer/Backend/SynchronizationFrame.h>
#include <Veng/Renderer/Backend/TypeMapping.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/TimelineSemaphore.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    Ref<Buffer> MakeScratch(Context& context)
    {
        return Buffer::Create(context, {
                                           .Name = "Transfer Scratch",
                                           .Size = 256,
                                           .Usage = BufferUsage::TransferSrc,
                                       });
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ReleaseBuffer nulls the wrapper so its destructor is a no-op")
{
    auto buffer = MakeScratch(Context);

    const auto released = ReleaseBuffer(*buffer);

    // The wrapper no longer owns the handles.
    CHECK(released.Buffer);
    CHECK(GetVkBuffer(*buffer) == vk::Buffer{});
    CHECK(GetVmaAllocation(*buffer) == nullptr);

    // Dropping the wrapper must NOT retire the (now-null) handle into a frame
    // bin — if it did, the bin would later vmaDestroyBuffer a handle this test
    // still owns. Hand it to the transfer path keyed on value 0 (always reached)
    // so the fixture teardown reclaims it cleanly.
    buffer.reset();

    Context.GetNative().RetireOnTransfer(released.Buffer, released.Allocation, 0);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "RetireOnTransfer holds the allocation until the timeline reaches its value")
{
    // The drain compares against the Context's own transfer timeline, which
    // starts at 0 and is unsignalled in this fixture. Retire against value 1
    // (unreached), drain, then signal value 1 and drain again.
    auto buffer = MakeScratch(Context);
    const auto released = ReleaseBuffer(*buffer);
    buffer.reset();

    constexpr u64 value = 1;
    Context.GetNative().RetireOnTransfer(released.Buffer, released.Allocation, value);

    // Timeline still at 0 (< value): the drain must not free it.
    Context.GetNative().DrainTransferRetireList();
    CHECK(Context.GetNative().TransferRetireList.size() == 1);

    // Timeline now at value: the drain reclaims it.
    Context.GetNative().TransferTimeline->Signal(value);
    Context.GetNative().DrainTransferRetireList();
    CHECK(Context.GetNative().TransferRetireList.empty());
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "teardown drains the transfer-retire list (upload-then-immediate-dispose)")
{
    // No frame is ever rendered here: a buffer is released into the transfer
    // list and ~Context (WaitIdle -> drain retire bins + transfer scratch) must
    // reclaim it without leaking or tripping the Disposed assert.
    auto buffer = MakeScratch(Context);
    const auto released = ReleaseBuffer(*buffer);
    buffer.reset();

    Context.GetNative().RetireOnTransfer(released.Buffer, released.Allocation, 0);

    REQUIRE(Context.GetNative().TransferRetireList.size() == 1);
    // The fixture teardown does the rest; nothing here must call DrainTransfer.
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "AddFrameTransferWait folds into a headless frame submit")
{
    // Signal the wait value first so the folded-in timeline wait is already
    // satisfied — the headless submit must complete (the fixture WaitIdle in
    // teardown would otherwise hang) and produce no validation error.
    auto timeline = TimelineSemaphore::Create(Context, 0);
    timeline->Signal(5);

    Context.AddFrameTransferWait(*timeline, 5);

    Context.BeginFrame();
    Context.EndFrame();

    Context.WaitIdle();
}

namespace
{
    // Total buffer handles across every per-slot retire bin (the pending bin excluded).
    usize BinnedBufferCount(const Context::Native& native)
    {
        usize count = 0;
        for (const auto& bin : native.RetireBins)
        {
            count += bin.Buffers.size();
        }
        return count;
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "a buffer dropped between frames is protected by the next frame, not the acquire drain")
{
    // The bug this guards: the standard frame loop drops resources before BeginFrame (a
    // per-frame update phase precedes it), when CurrentFrameInFlight already names the slot
    // AcquireNextFrame is about to wait-and-drain. Binning such a drop directly into that slot
    // lets the drain destroy it — even though it was last referenced by the previously submitted
    // frame's command buffer, guarded by a strictly later fence. The buffer must instead survive
    // until the frame about to record has itself completed.
    const Context::Native& native = Context.GetNative();

    // A frame the buffer is conceptually used by: record and submit it, then advance past it.
    Context.BeginFrame();
    Context.EndFrame();

    // Between EndFrame and the next BeginFrame — exactly where the frame loop's update phase
    // drops resources. FrameRecording is false here.
    CHECK_FALSE(native.FrameRecording);
    auto buffer = MakeScratch(Context);
    buffer.reset();

    // The drop must NOT land in the slot AcquireNextFrame will drain next; it parks pending.
    CHECK(BinnedBufferCount(native) == 0);
    CHECK(native.PendingRetire.Buffers.size() == 1);

    // The next BeginFrame drains the acquired slot, then folds the pending drop into that slot's
    // bin — so the buffer is now guarded by this recording frame's fence and survived the drain.
    Context.BeginFrame();
    CHECK(native.PendingRetire.Buffers.empty());
    CHECK(BinnedBufferCount(native) == 1);
    Context.EndFrame();

    // Cycle every slot back around: the guarding fence signals and the drain reclaims the buffer.
    for (u32 i = 0; i <= Context.GetMaxFramesInFlight(); ++i)
    {
        Context.BeginFrame();
        Context.EndFrame();
    }
    Context.WaitIdle();
    CHECK(BinnedBufferCount(native) == 0);
}

namespace
{
    // Descriptors in the loop's one binding; with the iteration count below, the loop allocates
    // eight times the pool's uniform-buffer budget.
    constexpr u32 DescriptorsPerSet = 64;

    // A uniform-buffer layout: MoltenVK enforces the pool's budget for buffer descriptors (it runs
    // dry at a few times the nominal count) but not for image or sampler descriptors.
    Ref<DescriptorSetLayout> MakeBufferLayout(Context& context)
    {
        return DescriptorSetLayout::Create(context, {
                                                        .Name = "Retire Loop Layout",
                                                        .Bindings = {{
                                                            .Binding = 0,
                                                            .Type = DescriptorType::UniformBuffer,
                                                            .Count = DescriptorsPerSet,
                                                            .Stages = ShaderStage::Compute,
                                                        }},
                                                    });
    }

    // Drops a buffer outside any frame, so it parks in the pending bin.
    void RetireBufferOutsideFrame(Context& context)
    {
        auto buffer = MakeScratch(context);
        buffer.reset();
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "immediate submits outside a frame keep the pending bin and descriptor pool bounded")
{
    // A process that never begins a frame retires everything into the pending bin. Each
    // iteration allocates and drops a descriptor set and a buffer inside a one-off submit; the
    // pool runs dry unless the drops are released between submits.
    const Context::Native& native = Context.GetNative();
    const auto layout = MakeBufferLayout(Context);
    const u32 iterations =
        8 * GetDescriptorTypeInfo(DescriptorType::UniformBuffer).PoolBudget / DescriptorsPerSet;

    // One iteration retires one set and one buffer; a bin holding that many after a submit has
    // carried a previous submit's drops forward.
    constexpr usize RetiredPerIteration = 2;
    usize peakPending = 0;
    u32 completed = 0;
    for (u32 i = 0; i < iterations; ++i)
    {
        Context.ImmediateCommands(
            [&](CommandBuffer&)
            {
                auto set = DescriptorSet::Create(Context, {
                                                              .Name = "Retire Loop Set",
                                                              .Layout = layout,
                                                          });
                auto buffer = MakeScratch(Context);
                set.reset();
                buffer.reset();
            });
        peakPending = std::max(peakPending, native.PendingRetire.Size());
        // Stop at the first violation, ahead of the pool assert it would lead to.
        if (peakPending >= RetiredPerIteration)
        {
            break;
        }
        ++completed;
    }

    CHECK(completed == iterations);
    CHECK(peakPending < RetiredPerIteration);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "an immediate submit keeps the pending bin while a frame slot is in flight")
{
    // A frame slot whose fence is unsignaled may still read what was dropped since its EndFrame.
    // Unsignaling a slot other than the current one stands in for that frame without stalling
    // the immediate submit, which waits only the current slot.
    Context::Native& native = Context.GetNative();
    REQUIRE(native.MaxFramesInFlight >= 2);
    const u32 inFlightSlot = (native.CurrentFrameInFlight + 1) % native.MaxFramesInFlight;
    const vk::Fence fence =
        GetVkFence(native.SynchronizationFrames[inFlightSlot].GetInFlightFence());
    VK_ASSERT(GetVkDevice(Context).resetFences(fence), "failed to reset fence");

    RetireBufferOutsideFrame(Context);
    Context.ImmediateCommands([](CommandBuffer&) {});
    CHECK(native.PendingRetire.Buffers.size() == 1);

    // The slot's work completes: the next immediate submit releases the buffer.
    native.LockedSubmit(native.GraphicsQueue, vk::SubmitInfo{}, fence);
    native.SynchronizationFrames[inFlightSlot].GetInFlightFence().Wait();
    Context.ImmediateCommands([](CommandBuffer&) {});
    CHECK(native.PendingRetire.Size() == 0);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "an immediate submit keeps the pending bin while transfer work is in flight")
{
    // A transfer value submitted but not reached is work that may still write a resource dropped
    // since. Allocating the value without signalling it stands in for that submit.
    Context::Native& native = Context.GetNative();
    u64 value = 0;
    {
        const std::scoped_lock lock(native.SubmitMutex);
        value = ++native.TransferTimelineValue;
    }

    RetireBufferOutsideFrame(Context);
    Context.ImmediateCommands([](CommandBuffer&) {});
    CHECK(native.PendingRetire.Buffers.size() == 1);

    native.TransferTimeline->Signal(value);
    Context.ImmediateCommands([](CommandBuffer&) {});
    CHECK(native.PendingRetire.Size() == 0);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "an immediate submit nested in another leaves the pending bin to the outer one")
{
    // The outer recording is not yet submitted when the inner one completes, so what was dropped
    // before the inner call may still be used by commands the outer buffer holds.
    const Context::Native& native = Context.GetNative();
    usize pendingAfterInner = 0;
    Context.ImmediateCommands(
        [&](CommandBuffer&)
        {
            RetireBufferOutsideFrame(Context);
            Context.ImmediateCommands([](CommandBuffer&) {});
            pendingAfterInner = native.PendingRetire.Buffers.size();
        });
    CHECK(pendingAfterInner == 1);
    CHECK(native.PendingRetire.Size() == 0);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "an immediate submit outside a frame frees upload scratch the transfer reached")
{
    auto buffer = MakeScratch(Context);
    const auto released = ReleaseBuffer(*buffer);
    buffer.reset();

    Context::Native& native = Context.GetNative();
    native.RetireOnTransfer(released.Buffer, released.Allocation, 0);
    REQUIRE(native.TransferRetireList.size() == 1);

    Context.ImmediateCommands([](CommandBuffer&) {});
    CHECK(native.TransferRetireList.empty());
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "an immediate submit outside a frame acquires a queued bindless view")
{
    // The state an async upload leaves: written on the transfer queue, its wait value reached.
    Context::Native& native = Context.GetNative();
    u64 value = 0;
    {
        const std::scoped_lock lock(native.SubmitMutex);
        value = ++native.TransferTimelineValue;
    }
    native.TransferTimeline->Signal(value);

    const Ref<Image> image =
        Image::Create(Context, {
                                   .Name = "Queued Acquire",
                                   .Extent = {4, 4, 1},
                                   .Format = Format::RGBA8Unorm,
                                   .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                               });
    Ref<ImageView> view =
        ImageView::Create(Context, {.Name = "Queued Acquire View", .Image = image});

    // Laid out by a real copy first, so the acquire transitions from a defined layout.
    Context.ImmediateCommands([&](CommandBuffer& cmd)
                              { Backend::TransitionImage(cmd, *image, ImageLayout::TransferDst); });
    Image::Native::SubresourceState& state = image->GetNative().At(0, 0);
    state.ProducingFamily =
        Context.GetQueueFamilies().TransferFamily.value_or(VK_QUEUE_FAMILY_IGNORED);
    state.PendingTransferValue = value;

    Context.EnqueueBindlessAcquire(view);
    const std::weak_ptr<ImageView> queued = view;
    view.reset();

    Context.ImmediateCommands([](CommandBuffer&) {});
    CHECK(state.Layout == vk::ImageLayout::eShaderReadOnlyOptimal);
    CHECK(state.PendingTransferValue == 0);
    CHECK(native.PendingFrameTransferWaits.empty());
    CHECK(queued.expired());
}
