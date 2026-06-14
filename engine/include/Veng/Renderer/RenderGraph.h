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
// layout type. The graph is rebuilt per frame (it is just a vector of pass structs).
namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;
    class RenderGraph;

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

    private:
        friend class RenderGraph;

        PassContext(CommandBuffer& cmd, const vector<Ref<ImageView>>& resolved)
            : m_Cmd(cmd), m_Resolved(resolved)
        {
        }

        CommandBuffer& m_Cmd;
        // The graph's resource table resolved to concrete views, indexed by
        // ResourceId::Index. Owned by the executing RenderGraph for the call.
        const vector<Ref<ImageView>>& m_Resolved;
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

        // Resolve transients, bind the supplied imports, derive the barriers, and run
        // each pass's Execute in order. Every declared import must appear in
        // `imports` (asserts otherwise); a graph with no imports takes an empty list.
        void Execute(CommandBuffer& cmd, std::span<const ImportBinding> imports = {});

    private:
        // A resource-table entry: either a graph-owned transient (allocated lazily
        // from its TransientDesc) or an import (resolved from the per-call binding).
        struct Resource
        {
            bool IsImport = false;
            string Name;
            TransientDesc Desc;            // transient only
            Ref<Image> Image;              // transient backing, allocated on first use
            Ref<ImageView> View;           // transient view, allocated on first use
        };

        // The context this graph allocates transients with (deferred-destruction
        // back-ref; the graph and its transients must not outlive the context).
        Context& m_Context;

        vector<Resource> m_Resources;

        // Heap-allocated passes so a PassBuilder's reference stays valid as more
        // passes are appended.
        vector<Unique<Pass>> m_Passes;
    };
}
