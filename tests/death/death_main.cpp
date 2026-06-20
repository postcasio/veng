// Death-test harness.
//
// VE_ASSERT calls std::abort(), which doctest cannot trap in-process, so death
// cases run as separate processes: ctest invokes this binary with a case name
// as argv[1], the case performs exactly the offending operation, and the
// process is expected to abort with a matching assert message.
//
// Registration lives in CMakeLists.txt — one add_test per case with
// PASS_REGULAR_EXPRESSION pinned to that case's assert message. A case passes
// iff the expected message reaches stderr: a case that aborts for the *wrong*
// reason, or fails to abort at all (a clean exit, or an unknown/missing case
// name), never matches and so fails loudly.
//
// Why a SIGABRT handler instead of WILL_FAIL: VE_ASSERT calls std::abort(),
// which terminates the process by *signal*. CTest reports a signal death as
// "Subprocess aborted" — a failure that neither WILL_FAIL nor a matching
// PASS_REGULAR_EXPRESSION overrides (verified against this CMake). So we trap
// SIGABRT and convert the death into a controlled clean exit *after* the assert
// message is already on stderr; the message match is then what decides pass/
// fail.
//
// Cases come in two bands:
//   - Pure-logic: no Context, run with no ICD (registered `death`).
//   - GPU-coupled: need a headless Context; registered `gpu;death` with
//     SKIP_RETURN_CODE so they report *skipped* (not failed) on a machine with
//     no Vulkan driver, via HasVulkanDriver().

#include <csignal>
#include <cstdlib>
#include <span>
#include <string_view>

#include <Veng/Assert.h>
#include <Veng/Log.h>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/TypedBuffers.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/VertexBufferLayout.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <support/GpuProbe.h>

#include <fmt/format.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // ECS death-case fixtures: a component type, plus two distinct types that
    // deliberately claim the same TypeId to exercise the collision assert.
    struct DeathPosition { f32 X = 0.0f; };
    struct CollideA { int Value = 0; };
    struct CollideB { float Value = 0.0f; };
}

VE_TYPE(DeathPosition, 0x45680D614D2A8FE4ULL);
VE_TYPE(CollideA, 0xB87E1263116E0707ULL);
VE_TYPE(CollideB, 0xB87E1263116E0707ULL); // same id as CollideA — a collision

namespace
{
    // Exit code that marks a GPU-coupled case as skipped (no Vulkan driver). Must
    // match SKIP_RETURN_CODE in CMakeLists.txt. CTest's skip detection takes
    // precedence over PASS_REGULAR_EXPRESSION, so a driverless run reports
    // *skipped* rather than failing on a missed message.
    constexpr int SkipExitCode = 77;

    // Lands here once VE_ASSERT has already logged + flushed its message to
    // stderr, on whichever signal the fatal path raised. Convert it into a clean
    // exit so CTest judges the run by PASS_REGULAR_EXPRESSION (the assert message)
    // rather than reporting an un-overridable "Subprocess aborted"/signal death.
    // std::_Exit is async-signal-safe and skips atexit/global-dtor teardown,
    // which is what we want after a fatal assert.
    //
    // Three signals, because FatalAssert's path differs by build:
    //  - default build: std::abort() -> SIGABRT.
    //  - VE_DEBUG build: VE_DEBUG_BREAK() fires *before* abort — __builtin_debugtrap
    //    (clang) raises SIGTRAP, __builtin_trap (gcc) raises SIGILL — so with no
    //    debugger attached the process would die there first. Trapping these lets
    //    the death band stay green under the validation build too.
    // Only the fatal-assert path produces these here; a real crash (SIGSEGV) is
    // left to fail, and the message match still gates a pass either way.
    [[noreturn]] void OnFatalSignal(int)
    {
        std::_Exit(0);
    }

    // Route assert output to unbuffered stderr so the message survives abort()
    // (std::abort does not flush buffered stdio) and ctest can match it. The
    // default log sink writes to (buffered) stdout, which abort() would discard.
    void InstallStderrSink()
    {
        Log::SetSink([](Log::Level, std::string_view message)
        {
            fmt::print(stderr, "{}\n", message);
        });
    }

    // -- Pure-logic death cases (no device) ----------------------------------

    void RunSentinel()
    {
        VE_ASSERT(false, "sentinel death case");
    }

    void RunVertexFormatUnknown()
    {
        // RGBA8Unorm is not a vertex element format; GetFormatSize aborts while
        // the layout's element is constructed.
        const VertexBufferLayout layout = {{Format::RGBA8Unorm, "bad"}};
        (void)layout.GetStride();
    }

    void RunToVkUnmapped()
    {
        // A Format value outside the mapped range hits ToVk's "unmapped" assert.
        volatile auto bad = static_cast<Format>(200);
        const vk::Format mapped = ToVk(static_cast<Format>(bad));
        (void)mapped;
    }

    void RunAssertMessage()
    {
        // Proves FatalAssert routes the formatted message to the log sink before
        // aborting.
        VE_ASSERT(false, "assert_message case fired");
    }

    // -- ECS death cases (pure-logic, no device) -----------------------------

    void RunSceneGetStaleEntity()
    {
        TypeRegistry registry;
        registry.Register<DeathPosition>("DeathPosition");
        const Unique<Scene> scene = Scene::Create(registry);

        const Entity e = scene->CreateEntity();
        scene->Add<DeathPosition>(e);
        scene->DestroyEntity(e);

        // e's slot may be recycled, but its generation has been bumped, so the
        // stale handle fails the IsAlive assert.
        (void)scene->Get<DeathPosition>(e);
    }

    void RunSceneGetMissingComponent()
    {
        TypeRegistry registry;
        registry.Register<DeathPosition>("DeathPosition");
        const Unique<Scene> scene = Scene::Create(registry);

        const Entity e = scene->CreateEntity();
        // The entity is live but has no DeathPosition: Get asserts present.
        (void)scene->Get<DeathPosition>(e);
    }

    void RunTypeIdCollision()
    {
        TypeRegistry registry;
        registry.Register<CollideA>("CollideA");
        // CollideB claims CollideA's id — a fatal collision assert.
        registry.Register<CollideB>("CollideB");
    }

    TypeRegistry MakeTransformRegistry()
    {
        TypeRegistry registry;
        registry.Register<Name>("Name");
        registry.Register<Transform>("Transform");
        registry.Register<Parent>("Parent");
        return registry;
    }

    void RunTransformParentCycle()
    {
        TypeRegistry registry = MakeTransformRegistry();
        const Unique<Scene> scene = Scene::Create(registry);

        const Entity a = scene->CreateEntity();
        const Entity b = scene->CreateEntity();
        scene->Add<Transform>(a);
        scene->Add<Transform>(b);
        // a → b → a forms a cycle; the walk detects revisiting an entity.
        scene->Add<Parent>(a, Parent{b});
        scene->Add<Parent>(b, Parent{a});

        (void)WorldMatrix(*scene, a);
    }

    void RunTransformParentDead()
    {
        TypeRegistry registry = MakeTransformRegistry();
        const Unique<Scene> scene = Scene::Create(registry);

        const Entity child = scene->CreateEntity();
        const Entity parent = scene->CreateEntity();
        scene->Add<Transform>(child);
        scene->Add<Parent>(child, Parent{parent});
        // Kill the parent: the walk hits a Parent referencing a dead entity.
        scene->DestroyEntity(parent);

        (void)WorldMatrix(*scene, child);
    }

    // -- Reflection serializer death cases (pure-logic, no device) -----------
    //
    // The serializer's contract on malformed bytes is a loud fatal assert, never
    // silent UB. Each case truncates a different guard: the leading record count,
    // a field value region, and a string body whose declared length overruns its
    // record.

    void RunSerializeTruncatedHeader()
    {
        TypeRegistry registry;
        registry.Register<Transform>();

        // Fewer than four bytes: the very first ReadU32 (the record count) aborts.
        const u8 bytes[2] = {0x01, 0x00};
        Transform dst;
        ReadFields(std::span<const u8>(bytes, sizeof(bytes)), &dst,
                   registry.Info(registry.IdOf<Transform>()), registry);
    }

    void RunSerializeTruncatedValue()
    {
        TypeRegistry registry;
        registry.Register<Transform>();
        const TypeInfo& info = registry.Info(registry.IdOf<Transform>());

        Transform src;
        vector<u8> bytes;
        WriteFields(bytes, &src, info, registry);

        // Cut into the last field's value region: the record's declared value
        // length now exceeds what remains, tripping the field-value guard.
        bytes.resize(bytes.size() - 8);
        Transform dst;
        ReadFields(bytes, &dst, info, registry);
    }

    void RunSerializeTruncatedString()
    {
        TypeRegistry registry;
        registry.Register<Name>();
        const TypeInfo& info = registry.Info(registry.IdOf<Name>());

        auto pushU32 = [](vector<u8>& out, u32 value)
        {
            const auto* p = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), p, p + sizeof(value));
        };

        // A hand-built Name record: one field "Value", a value region of exactly
        // four bytes that declares a 100-char string — but no string bytes follow.
        // The string guard catches the overrun.
        vector<u8> bytes;
        pushU32(bytes, 1);                 // record count
        pushU32(bytes, 5);                 // name length
        bytes.insert(bytes.end(), {'V', 'a', 'l', 'u', 'e'});
        pushU32(bytes, 4);                 // value length (just the inner length prefix)
        pushU32(bytes, 100);               // inner string length — overruns the record

        Name dst;
        ReadFields(bytes, &dst, info, registry);
    }

    // -- GPU-coupled death cases (need a headless Context) -------------------

    // Bring up a headless Context, run `body` (which is expected to abort), and
    // never return: skip if no driver, fail loudly if `body` somehow returns.
    template <typename Body>
    [[noreturn]] void InGpuContext(Body&& body)
    {
        if (!Test::HasVulkanDriver())
            std::_Exit(SkipExitCode);

        Context context;
        context.Initialize({
            .ApplicationName = "Death Test",
            .InternalRenderExtent = {4, 4},
        }, nullptr);

        body(context);

        // body was supposed to abort; reaching here means it did not.
        fmt::print(stderr, "death harness: GPU case did not abort\n");
        std::_Exit(1);
    }

    void RunBufferUploadOverrun()
    {
        InGpuContext([](Context& context)
        {
            const auto buffer = Buffer::Create(context, {
                .Name = "overrun",
                .Size = 16,
                .Usage = BufferUsage::TransferDst,
            });
            const u8 data[32] = {};
            buffer->UploadSync({data, sizeof(data)}); // offset 0 + 32 > 16
        });
    }

    void RunIndexU16IntoU32()
    {
        InGpuContext([](Context& context)
        {
            const auto index = IndexBuffer::Create(context, "indices", 4, IndexType::U32);
            const u16 values[4] = {};
            index.UploadSync(std::span<const u16>(values)); // buffer is U32
        });
    }

    void RunIndexU32IntoU16()
    {
        InGpuContext([](Context& context)
        {
            const auto index = IndexBuffer::Create(context, "indices", 4, IndexType::U16);
            const u32 values[4] = {};
            index.UploadSync(std::span<const u32>(values)); // buffer is U16
        });
    }

    void RunDescriptorTypeMismatch()
    {
        InGpuContext([](Context& context)
        {
            const auto layout = DescriptorSetLayout::Create(context, {
                .Name = "mismatch-layout",
                .Bindings = {{
                    .Binding = 0,
                    .Type = DescriptorType::UniformBuffer,
                    .Count = 1,
                    .Stages = ShaderStage::Fragment,
                }},
            });
            const auto set = DescriptorSet::Create(context, {.Name = "mismatch-set", .Layout = layout});

            const auto image = Image::Create(context, {
                .Name = "img",
                .Extent = {4, 4, 1},
                .Format = Format::RGBA8Unorm,
                .Usage = ImageUsage::Sampled,
            });
            const auto view = ImageView::Create(context, {.Name = "iv", .Image = image});

            // Binding 0 is a UniformBuffer; the image Write asserts the type.
            set->Write(0, view);
        });
    }
}

int main(int argc, char** argv)
{
    InstallStderrSink();
    std::signal(SIGABRT, OnFatalSignal); // std::abort (default build)
    std::signal(SIGTRAP, OnFatalSignal); // __builtin_debugtrap (clang, VE_DEBUG)
    std::signal(SIGILL, OnFatalSignal);  // __builtin_trap (gcc, VE_DEBUG)

    if (argc < 2)
    {
        fmt::print(stderr, "death harness: no case name given\n");
        return 1;
    }

    const std::string_view name = argv[1];

    // Pure-logic
    if (name == "sentinel") RunSentinel();
    else if (name == "vertex_format_unknown") RunVertexFormatUnknown();
    else if (name == "tovk_unmapped") RunToVkUnmapped();
    else if (name == "assert_message") RunAssertMessage();
    else if (name == "scene_get_stale_entity") RunSceneGetStaleEntity();
    else if (name == "scene_get_missing_component") RunSceneGetMissingComponent();
    else if (name == "type_id_collision") RunTypeIdCollision();
    else if (name == "transform_parent_cycle") RunTransformParentCycle();
    else if (name == "transform_parent_dead") RunTransformParentDead();
    else if (name == "serialize_truncated_header") RunSerializeTruncatedHeader();
    else if (name == "serialize_truncated_value") RunSerializeTruncatedValue();
    else if (name == "serialize_truncated_string") RunSerializeTruncatedString();
    // GPU-coupled
    else if (name == "buffer_upload_overrun") RunBufferUploadOverrun();
    else if (name == "index_u16_into_u32") RunIndexU16IntoU32();
    else if (name == "index_u32_into_u16") RunIndexU32IntoU16();
    else if (name == "descriptor_type_mismatch") RunDescriptorTypeMismatch();
    else
        fmt::print(stderr, "death harness: unknown case '{}'\n", name);

    // Reached only if the case did not abort — exit non-zero, and (with no
    // assert message printed) the PASS_REGULAR_EXPRESSION misses: the test fails.
    return 1;
}
