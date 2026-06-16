#pragma once

#include <functional>
#include <span>

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/ImageView.h>

// A linear render graph: passes are declared with the logical resources they read
// and write, and the engine derives every pipeline barrier from those declarations.
// Passes execute in declaration order — no culling, reordering, or aliasing.
//
// Resources are addressed by a vk-free ResourceId, not a concrete Ref<ImageView>:
// a transient is graph-owned (declared with CreateTransient, allocated and resolved
// by the graph), an import is an external concrete resource (declared with Import,
// its view supplied per frame to Execute). A pass callback receives a PassContext
// that resolves a declared transient to its concrete view for this frame.
//
// Graphics passes get BeginRendering/EndRendering driven for them from their
// Color/Depth declarations; the Execute callback only binds and draws. This replaces
// manual ImageBarrier/layout management entirely — there is no public barrier or
// layout type.
//
// RenderGraph is a pure builder. Declaring passes does not execute anything; call
// Compile() once to derive the barrier/transition schedule, allocate transients,
// build the per-graphics-pass RenderingInfo, and run one-time validation. The
// returned CompiledGraph replays that schedule per frame via Execute. A structural
// change (a pass added/removed, a transient's extent/format changed) is a consumer-
// driven rebuild + re-Compile(); per-frame data never recompiles.
namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;
    class RenderGraph;
    class CompiledGraph;

    // A handle into a render graph's resource table — either a graph-owned transient
    // (allocated and resolved by the graph) or an imported external resource. Opaque;
    // produced only by RenderGraph::CreateTransient / Import.
    struct ResourceId
    {
        static constexpr u32 InvalidIndex = ~0u;
        u32 Index = InvalidIndex;
        [[nodiscard]] bool IsValid() const { return Index != InvalidIndex; }
    };

    // A graph-owned transient: a logical image with no backing until the graph
    // allocates it. Extent/format/usage are the allocation inputs.
    struct TransientDesc
    {
        string Name;
        Format Format = Format::Undefined;
        uvec2 Extent = {};
        ImageUsage Usage; // required, e.g. ImageUsage::ColorAttachment | ImageUsage::Sampled
    };

    // The per-frame record-time channel handed to a pass callback. Resolves a
    // declared transient to its concrete view for this frame.
    class PassContext
    {
    public:
        [[nodiscard]] CommandBuffer& Cmd() const { return m_Cmd; }

        // The concrete view a declared transient resolves to this frame. Asserts if
        // the id is not a resource resolved for this graph.
        [[nodiscard]] ImageView& Resolved(ResourceId id) const;

        // An opaque per-Execute pointer the caller threads through to pass
        // callbacks; null unless the Execute call set it. RenderGraph never reads
        // its target — a Scene-aware layer sets it and reads it back through a typed
        // wrapper, keeping the graph free of any dependency on what it points at.
        [[nodiscard]] void* UserData() const { return m_UserData; }

    private:
        friend class RenderGraph;
        friend class CompiledGraph;

        PassContext(CommandBuffer& cmd, const vector<Ref<ImageView>>& resolved, void* userData)
            : m_Cmd(cmd), m_Resolved(resolved), m_UserData(userData)
        {
        }

        CommandBuffer& m_Cmd;
        // The graph's resource table resolved to concrete views, indexed by
        // ResourceId::Index. Owned by the executing RenderGraph for the call.
        const vector<Ref<ImageView>>& m_Resolved;
        void* m_UserData = nullptr;
    };

    struct PassAttachment
    {
        ResourceId Resource;
        LoadOp Load = LoadOp::Clear;
        StoreOp Store = StoreOp::Store;
        ClearValue Clear = ClearColor{};
    };

    class RenderGraph
    {
    public:
        explicit RenderGraph(Context& context) : m_Context(context) {}

        enum class PassType : u8 { Graphics, Compute, Transfer };

        // How a pass uses a resource drives barrier derivation; the AccessKind
        // vocabulary enum lives in Types.h (it is also used by
        // CommandBuffer::PrepareForAccess for out-of-graph consumers).
        struct Access
        {
            ResourceId Resource;
            AccessKind Kind;
            LoadOp Load = LoadOp::Clear;       // attachments only
            StoreOp Store = StoreOp::Store;    // attachments only
            ClearValue Clear = ClearColor{};   // attachments only
        };

        struct Pass
        {
            string Name;
            PassType Type = PassType::Graphics;
            vector<Access> Accesses;
            u32 LayerCount = 1;
            u32 ViewMask = 0;
            function<void(PassContext&)> Execute;
        };

        class PassBuilder
        {
        public:
            explicit PassBuilder(Pass& pass) : m_Pass(pass) {}

            PassBuilder& Color(const PassAttachment& attachment);
            PassBuilder& Depth(const PassAttachment& attachment);
            PassBuilder& Sample(ResourceId resource);
            PassBuilder& StorageRead(ResourceId resource);
            PassBuilder& StorageWrite(ResourceId resource);
            PassBuilder& TransferSrc(ResourceId resource);
            PassBuilder& TransferDst(ResourceId resource);
            PassBuilder& LayerCount(u32 layerCount);
            PassBuilder& ViewMask(u32 viewMask);
            PassBuilder& Execute(function<void(PassContext&)> execute);

        private:
            Pass& m_Pass;
        };

        // Declare a graph-owned transient. Returns a handle usable in
        // Color/Depth/Sample/…
        [[nodiscard]] ResourceId CreateTransient(const TransientDesc& desc);

        // Declare an external resource (swapchain image, app-owned target). The graph
        // never allocates or aliases it; its concrete view is supplied per frame as a
        // binding to Execute. Returns a handle.
        [[nodiscard]] ResourceId Import(string_view name);

        // One import's concrete view for this frame, passed to Execute.
        struct ImportBinding
        {
            ResourceId Id;
            Ref<ImageView> View;
        };

        // Graphics pass (dynamic rendering). Declare Color/Depth attachments and
        // any Sample/Storage reads, then Execute to bind pipelines and draw.
        PassBuilder AddPass(string_view name);
        PassBuilder AddComputePass(string_view name);
        PassBuilder AddTransferPass(string_view name);

        // Compile the declared passes into a replayable graph: allocate transients,
        // derive the barrier/transition schedule, build each graphics pass's
        // RenderingInfo skeleton, and run one-time validation. Single owner — nothing
        // holds a Ref to a CompiledGraph → Unique, per docs/ownership.md.
        [[nodiscard]] Unique<CompiledGraph> Compile();

    private:
        // A resource-table entry: either a graph-owned transient (allocated at
        // Compile from its TransientDesc) or an import (resolved per frame from the
        // call's binding).
        struct Resource
        {
            bool IsImport = false;
            string Name;
            TransientDesc Desc; // transient only
        };

        // The context this graph allocates transients with (deferred-destruction
        // back-ref; the graph and its transients must not outlive the context).
        Context& m_Context;

        vector<Resource> m_Resources;

        // Heap-allocated passes so a PassBuilder's reference stays valid as more
        // passes are appended.
        vector<Unique<Pass>> m_Passes;
    };

    // A compiled, replayable render graph. Built by RenderGraph::Compile(); replayed
    // each frame. Owns its transient images; retires them through the per-frame
    // deferred-destruction path on destruction.
    class CompiledGraph
    {
    public:
        ~CompiledGraph();

        CompiledGraph(const CompiledGraph&) = delete;
        CompiledGraph& operator=(const CompiledGraph&) = delete;

        // Replay the baked schedule: resolve transients, bind the supplied imports,
        // emit the scheduled transitions through the tracked-state barrier path, drive
        // rendering, run each pass callback. Every declared import must appear in
        // `imports` (asserts otherwise); a graph with no imports takes {}. `userData`
        // is forwarded verbatim to every pass callback's PassContext::UserData();
        // null leaves it null.
        void Execute(CommandBuffer& cmd,
                     std::span<const RenderGraph::ImportBinding> imports = {},
                     void* userData = nullptr);

        // The concrete image a transient was allocated to at compile. Two
        // transients with non-overlapping lifetimes and an equal size class share
        // one image, so this returns the same Ref for both — the observable proof
        // that aliasing happened. Null for an import (no graph-owned backing) and
        // for an invalid id.
        [[nodiscard]] Ref<Image> ResolvedImage(ResourceId id) const;

    private:
        friend class RenderGraph;

        // The vk:: schedule + the graph-allocated transient images live in Native,
        // defined in RenderGraph.cpp — the public/backend split keeps this header
        // backend-free (guarded by the include_hygiene test).
        struct Native;
        explicit CompiledGraph(Unique<Native> native);

        Unique<Native> m_Native;
    };
}
