// Plan-05 plumbing: the two Context capabilities the async upload path needs.
//
// 1. RetireOnTransfer keys a raw allocation on the transfer timeline, not a
//    frame fence: the buffer is destroyed only once the timeline reaches the
//    value it was retired against, and the drain runs at teardown even with no
//    intervening frame (the upload-then-immediate-dispose case).
// 2. ReleaseBuffer hands out the raw (vk::Buffer, VmaAllocation) and nulls the
//    wrapper so its destructor does NOT route the handle through the frame bin.
// 3. AddFrameTransferWait accumulates a (timeline, value) the next frame submit
//    folds in — exercised through a headless SubmitFrame.
//
// These deal in raw Vulkan/VMA handles by design, so this case reaches through
// the public Native.h escape hatch; veng_gpu links Vulkan + VMA for it.

#include <doctest/doctest.h>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Context.h>
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

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "ReleaseBuffer nulls the wrapper so its destructor is a no-op")
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
    // list and the fixture's destructor (WaitIdle -> DisposeResources ->
    // Dispose) must reclaim it without leaking or tripping the Disposed assert.
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

    auto& commandBuffer = Context.BeginFrame();
    (void)commandBuffer;
    Context.EndFrame();

    Context.WaitIdle();
}
