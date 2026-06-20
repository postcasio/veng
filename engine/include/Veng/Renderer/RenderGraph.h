#pragma once

#include <functional>
#include <span>

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/ImageView.h>

/// @brief Barrier-deriving render graph for recording GPU passes.
///
/// Passes are declared with the logical resources they read and write; the engine
/// derives every pipeline barrier from those declarations. Passes execute in
/// declaration order — no culling, reordering, or aliasing.
///
/// Resources are addressed by a Vulkan-free ResourceId, not a concrete Ref<ImageView>:
/// a transient is graph-owned (declared with CreateTransient, allocated and resolved
/// by the graph); an import is an external concrete resource (declared with Import,
/// its view supplied per frame to Execute). A pass callback receives a PassContext
/// that resolves a declared transient to its concrete view for that frame.
///
/// Graphics passes receive BeginRendering/EndRendering from their Color/Depth
/// declarations; the Execute callback only binds and draws. This eliminates manual
/// ImageBarrier and layout management — there is no public barrier or layout type.
///
/// RenderGraph is a pure builder. Declaring passes executes nothing. Call Compile()
/// once to derive the barrier/transition schedule, allocate transients, build each
/// graphics pass's RenderingInfo, and run one-time validation. The returned
/// CompiledGraph replays that schedule per frame via Execute. A structural change (a
/// pass added/removed, a transient's extent/format changed) requires a consumer-driven
/// rebuild + re-Compile(); per-frame data never recompiles.
namespace Veng::Renderer
{
    class CommandBuffer;
    class Context;
    class RenderGraph;
    class CompiledGraph;

    /// @brief Opaque handle into a render graph's resource table.
    ///
    /// Either a graph-owned transient (allocated and resolved by the graph) or an
    /// imported external resource. Produced only by RenderGraph::CreateTransient or
    /// RenderGraph::Import.
    struct ResourceId
    {
        /// @brief Sentinel for an uninitialised handle.
        static constexpr u32 InvalidIndex = ~0u;
        /// @brief Index into the graph's resource table.
        u32 Index = InvalidIndex;
        /// @brief Returns true when this handle was produced by CreateTransient or Import.
        [[nodiscard]] bool IsValid() const { return Index != InvalidIndex; }
    };

    /// @brief Descriptor for a graph-owned transient image.
    ///
    /// A transient has no backing allocation until the graph's Compile step.
    /// Extent, format, and usage are the allocation inputs.
    struct TransientDesc
    {
        /// @brief Debug name for the transient image.
        string Name;
        /// @brief Pixel format of the transient.
        Format Format = Format::Undefined;
        /// @brief Width and height in texels.
        uvec2 Extent = {};
        /// @brief Required usage flags (e.g. ColorAttachment | Sampled).
        ImageUsage Usage;
    };

    /// @brief Per-frame record-time context handed to a pass callback.
    ///
    /// Resolves a declared transient to its concrete view for this frame and exposes
    /// the command buffer and the optional user-data pointer.
    class PassContext
    {
    public:
        /// @brief The command buffer for this pass.
        [[nodiscard]] CommandBuffer& Cmd() const { return m_Cmd; }

        /// @brief The concrete view a declared transient resolves to this frame.
        /// @pre id must be a resource resolved for this graph — asserted otherwise.
        [[nodiscard]] ImageView& Resolved(ResourceId id) const;

        /// @brief Opaque per-Execute pointer threaded through to every pass callback.
        ///
        /// Null unless the Execute call provided it. RenderGraph never reads its
        /// target — a Scene-aware layer sets it and reads it back through a typed
        /// wrapper, keeping the graph free of any dependency on what it points at.
        [[nodiscard]] void* UserData() const { return m_UserData; }

    private:
        friend class RenderGraph;
        friend class CompiledGraph;

        PassContext(CommandBuffer& cmd, const vector<Ref<ImageView>>& resolved, void* userData)
            : m_Cmd(cmd), m_Resolved(resolved), m_UserData(userData)
        {
        }

        CommandBuffer& m_Cmd;
        /// @brief Graph resource table resolved to concrete views, indexed by ResourceId::Index.
        const vector<Ref<ImageView>>& m_Resolved;
        /// @brief Forwarded verbatim from Execute.
        void* m_UserData = nullptr;
    };

    /// @brief Attachment descriptor for a Color or Depth pass slot.
    struct PassAttachment
    {
        /// @brief The graph resource bound to this attachment.
        ResourceId Resource;
        /// @brief Load operation at attachment begin.
        LoadOp Load = LoadOp::Clear;
        /// @brief Store operation at attachment end.
        StoreOp Store = StoreOp::Store;
        /// @brief Clear value used when Load is Clear.
        ClearValue Clear = ClearColor{};
    };

    /// @brief Barrier-deriving render graph builder.
    ///
    /// Declare passes and their resource accesses; call Compile() to produce a
    /// replayable CompiledGraph.
    class RenderGraph
    {
    public:
        /// @brief Constructs the graph bound to context, which allocates transients.
        explicit RenderGraph(Context& context) : m_Context(context) {}

        /// @brief Discriminator for the kind of GPU work a pass performs.
        enum class PassType : u8
        {
            /// @brief Dynamic-rendering pass with Color/Depth attachments.
            Graphics,
            /// @brief Compute dispatch pass.
            Compute,
            /// @brief Copy or blit pass.
            Transfer,
        };

        /// @brief A resource access declaration that drives barrier derivation.
        ///
        /// AccessKind lives in Types.h (also used by CommandBuffer::PrepareForAccess
        /// for out-of-graph consumers).
        struct Access
        {
            /// @brief The resource being accessed.
            ResourceId Resource;
            /// @brief How the pass uses the resource.
            AccessKind Kind;
            /// @brief Attachment load op (attachments only).
            LoadOp Load = LoadOp::Clear;
            /// @brief Attachment store op (attachments only).
            StoreOp Store = StoreOp::Store;
            /// @brief Clear value (attachments only).
            ClearValue Clear = ClearColor{};
        };

        /// @brief Internal representation of one declared pass.
        struct Pass
        {
            /// @brief Debug name.
            string Name;
            /// @brief Pass kind.
            PassType Type = PassType::Graphics;
            /// @brief Declared resource accesses.
            vector<Access> Accesses;
            /// @brief Layer count for multiview/array rendering.
            u32 LayerCount = 1;
            /// @brief Multiview mask (0 = no multiview).
            u32 ViewMask = 0;
            /// @brief Record-time callback.
            function<void(PassContext&)> Execute;
        };

        /// @brief Fluent builder for configuring a declared pass.
        class PassBuilder
        {
        public:
            /// @brief Constructs a builder over the given pass slot.
            explicit PassBuilder(Pass& pass) : m_Pass(pass) {}

            /// @brief Declares a color attachment access for this pass.
            PassBuilder& Color(const PassAttachment& attachment);
            /// @brief Declares a depth attachment access for this pass.
            PassBuilder& Depth(const PassAttachment& attachment);
            /// @brief Declares a shader-sample read on resource.
            PassBuilder& Sample(ResourceId resource);
            /// @brief Declares a storage-image read on resource.
            PassBuilder& StorageRead(ResourceId resource);
            /// @brief Declares a storage-image write on resource.
            PassBuilder& StorageWrite(ResourceId resource);
            /// @brief Declares a transfer-source read on resource.
            PassBuilder& TransferSrc(ResourceId resource);
            /// @brief Declares a transfer-destination write on resource.
            PassBuilder& TransferDst(ResourceId resource);
            /// @brief Sets the layer count for multiview or array rendering.
            PassBuilder& LayerCount(u32 layerCount);
            /// @brief Sets the multiview mask (0 disables multiview).
            PassBuilder& ViewMask(u32 viewMask);
            /// @brief Sets the record-time callback that binds and draws.
            PassBuilder& Execute(function<void(PassContext&)> execute);

        private:
            /// @brief The pass slot being built.
            Pass& m_Pass;
        };

        /// @brief Declares a graph-owned transient image.
        /// @return A handle usable in Color/Depth/Sample/… declarations.
        [[nodiscard]] ResourceId CreateTransient(const TransientDesc& desc);

        /// @brief Declares an external resource (swapchain image, app-owned target).
        ///
        /// The graph never allocates or aliases it; its concrete view is supplied
        /// per frame as an ImportBinding passed to Execute.
        /// @return A handle usable in access declarations.
        [[nodiscard]] ResourceId Import(string_view name);

        /// @brief Binds an imported resource's concrete view for one Execute call.
        struct ImportBinding
        {
            /// @brief The handle returned by Import.
            ResourceId Id;
            /// @brief The concrete view to resolve for this frame.
            Ref<ImageView> View;
        };

        /// @brief Adds a graphics pass (dynamic rendering).
        ///
        /// Declare Color/Depth attachments and any Sample/Storage reads via the
        /// returned builder, then set the Execute callback to bind pipelines and draw.
        PassBuilder AddPass(string_view name);
        /// @brief Adds a compute pass.
        PassBuilder AddComputePass(string_view name);
        /// @brief Adds a transfer pass.
        PassBuilder AddTransferPass(string_view name);

        /// @brief Compiles declared passes into a replayable CompiledGraph.
        ///
        /// Allocates transients, derives the barrier/transition schedule, builds each
        /// graphics pass's RenderingInfo skeleton, and runs one-time validation.
        /// Returns Unique because nothing holds a Ref to a CompiledGraph — it is
        /// single-owner by design.
        [[nodiscard]] Unique<CompiledGraph> Compile();

    private:
        /// @brief Resource-table entry: a graph-owned transient or an imported resource.
        struct Resource
        {
            /// @brief True for an imported (externally-owned) resource.
            bool IsImport = false;
            /// @brief Debug name.
            string Name;
            /// @brief Allocation descriptor (transient only).
            TransientDesc Desc;
        };

        /// @brief Context used to allocate transients; must outlive this graph.
        Context& m_Context;

        /// @brief The graph's resource table.
        vector<Resource> m_Resources;

        /// @brief Heap-allocated so a PassBuilder's reference stays valid as passes are appended.
        vector<Unique<Pass>> m_Passes;
    };

    /// @brief A compiled, replayable render graph.
    ///
    /// Built by RenderGraph::Compile(); replayed each frame via Execute. Owns its
    /// transient images and retires them through the per-frame deferred-destruction
    /// path on destruction.
    class CompiledGraph
    {
    public:
        /// @brief Retires owned transient images through the deferred-destruction path.
        ~CompiledGraph();

        CompiledGraph(const CompiledGraph&) = delete;
        CompiledGraph& operator=(const CompiledGraph&) = delete;

        /// @brief Replays the baked schedule for one frame.
        ///
        /// Resolves transients, binds the supplied imports, emits scheduled transitions
        /// through the tracked-state barrier path, drives rendering, and runs each pass
        /// callback. Every declared import must appear in `imports` (asserted otherwise);
        /// a graph with no imports passes {}. `userData` is forwarded verbatim to every
        /// pass callback's PassContext::UserData(); null leaves it null.
        /// @param cmd      Command buffer to record into.
        /// @param imports  Concrete views for each declared import this frame.
        /// @param userData Opaque value forwarded to PassContext::UserData.
        void Execute(CommandBuffer& cmd, std::span<const RenderGraph::ImportBinding> imports = {},
                     void* userData = nullptr);

        /// @brief Returns the concrete image a transient was allocated to at compile.
        ///
        /// Two transients with non-overlapping lifetimes and an equal size class share
        /// one image, so this returns the same Ref for both — the observable proof that
        /// aliasing occurred. Returns null for an import (no graph-owned backing) and
        /// for an invalid id.
        /// @param id  A ResourceId returned by RenderGraph::CreateTransient.
        [[nodiscard]] Ref<Image> ResolvedImage(ResourceId id) const;

    private:
        friend class RenderGraph;

        /// @brief The Vulkan schedule and graph-allocated transient images.
        ///
        /// Defined in RenderGraph.cpp — the public/backend split keeps this header
        /// backend-free (guarded by the include_hygiene test).
        struct Native;
        explicit CompiledGraph(Unique<Native> native);

        /// @brief Backend schedule and transient image storage.
        Unique<Native> m_Native;
    };
}
