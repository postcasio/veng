#pragma once

#include <functional>

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/ImageView.h>

// A linear render graph: passes are declared with the image views they read and
// write, and the engine derives every pipeline barrier from those declarations.
// Passes execute in declaration order — no culling, reordering, or aliasing.
//
// All access declarations take Ref<ImageView>, not Ref<Image>: the view carries
// the subresource range (mips/layers) that is the unit of barrier precision, so
// passes touching different mips/faces of one image get correct, narrow
// transitions. Graphics passes get BeginRendering/EndRendering driven for them
// from their Color/Depth declarations; the Execute lambda only binds and draws.
//
// This replaces manual ImageBarrier/layout management entirely — there is no
// public barrier or layout type anymore. The graph is rebuilt per frame (it is
// just a vector of pass structs); a compiled/retained graph is a later upgrade.
namespace Veng::Renderer
{
    class CommandBuffer;

    struct PassAttachment
    {
        Ref<ImageView> View;
        LoadOp Load = LoadOp::Clear;
        StoreOp Store = StoreOp::Store;
        ClearValue Clear = ClearColor{};
    };

    class RenderGraph
    {
    public:
        enum class PassType : u8 { Graphics, Compute, Transfer };

        // How a pass uses a view. Each maps internally to a (layout, stage,
        // access) triple that drives barrier derivation.
        enum class AccessKind : u8
        {
            ColorAttachment,
            DepthAttachment,
            Sample,
            StorageRead,
            StorageWrite,
            TransferSrc,
            TransferDst,
        };

        struct Access
        {
            Ref<ImageView> View;
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
            function<void(CommandBuffer&)> Execute;
        };

        class PassBuilder
        {
        public:
            explicit PassBuilder(Pass& pass) : m_Pass(pass) {}

            PassBuilder& Color(const PassAttachment& attachment);
            PassBuilder& Depth(const PassAttachment& attachment);
            PassBuilder& Sample(const Ref<ImageView>& view);
            PassBuilder& StorageRead(const Ref<ImageView>& view);
            PassBuilder& StorageWrite(const Ref<ImageView>& view);
            PassBuilder& TransferSrc(const Ref<ImageView>& view);
            PassBuilder& TransferDst(const Ref<ImageView>& view);
            PassBuilder& LayerCount(u32 layerCount);
            PassBuilder& ViewMask(u32 viewMask);
            PassBuilder& Execute(function<void(CommandBuffer&)> execute);

        private:
            Pass& m_Pass;
        };

        // Graphics pass (dynamic rendering). Declare Color/Depth attachments and
        // any Sample/Storage reads, then Execute to bind pipelines and draw.
        PassBuilder AddPass(string_view name);
        PassBuilder AddComputePass(string_view name);
        PassBuilder AddTransferPass(string_view name);

        // Emit the derived barriers and run each pass's Execute in order.
        void Execute(CommandBuffer& cmd);

    private:
        // Heap-allocated passes so a PassBuilder's reference stays valid as more
        // passes are appended.
        vector<Unique<Pass>> m_Passes;
    };
}
