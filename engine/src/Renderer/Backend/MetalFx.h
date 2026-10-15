#pragma once

// The MetalFX upscalers, wrapped for the renderer. Free of Vulkan and Metal types: the scalers read
// and write engine Images through their exported platform textures, and schedule their work
// between two segments of the frame with Context::SplitFrame.
//
// On Apple the declarations below are defined in MetalFx.mm, the one translation unit that names
// MetalFX. Elsewhere there is no MetalFX, and the inline definitions at the bottom of this header
// report it unsupported and create nothing.

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;
    class Image;
}

namespace Veng::Renderer::Backend
{
    /// @brief What the device's MetalFX can do, read once from the device.
    struct MetalFxSupport
    {
        /// @brief Whether a temporal scaler can be created on the device.
        bool Temporal = false;
        /// @brief Whether a spatial scaler can be created on the device.
        bool Spatial = false;
        /// @brief The smallest output-to-input-content ratio a temporal scaler accepts.
        f32 TemporalMinScale = 1.0f;
        /// @brief The largest output-to-input-content ratio a temporal scaler accepts.
        f32 TemporalMaxScale = 1.0f;
    };

    /// @brief Reads what MetalFX supports on the context's device.
    ///
    /// Everything false where the context cannot split a frame (no platform-object interop), on a
    /// system without the MetalFX framework, and on every platform but Apple.
    /// @param context The render context.
    /// @return The device's MetalFX support.
    [[nodiscard]] MetalFxSupport QueryMetalFxSupport(const Context& context);

    /// @brief The fixed shape of a temporal scaler: its texture sizes and formats.
    struct MetalFxTemporalInfo
    {
        /// @brief The size of the colour, depth and motion textures (the render allocation).
        uvec2 InputExtent = {1, 1};
        /// @brief The size of the output texture.
        uvec2 OutputExtent = {1, 1};
        /// @brief The largest output-to-content ratio a frame will encode at.
        f32 MaxScale = 1.0f;
        /// @brief Format of the jittered colour input.
        Format ColorFormat = Format::RGBA16Sfloat;
        /// @brief Format of the depth input.
        Format DepthFormat = Format::D32Sfloat;
        /// @brief Format of the motion-vector input.
        Format MotionFormat = Format::RG16Sfloat;
        /// @brief Format of the reconstructed output.
        Format OutputFormat = Format::RGBA16Sfloat;
        /// @brief Whether the depth input stores the far plane at zero.
        bool DepthReversed = true;
    };

    /// @brief One frame's encode of a temporal scaler.
    struct MetalFxTemporalFrame
    {
        /// @brief The jittered colour, InputExtent-sized; its top-left ContentExtent is valid.
        const Image* Color = nullptr;
        /// @brief The depth matching Color.
        const Image* Depth = nullptr;
        /// @brief The motion vectors matching Color.
        const Image* Motion = nullptr;
        /// @brief The OutputExtent-sized image the scaler reconstructs in full.
        const Image* Output = nullptr;
        /// @brief The rendered sub-rect of the inputs this frame.
        uvec2 ContentExtent = {1, 1};
        /// @brief How far the jitter moved the rendered image, in input pixels (x right, y down).
        vec2 JitterPixels{0.0f};
        /// @brief Converts a stored motion vector into input pixels pointing at the previous frame.
        vec2 MotionScale{1.0f};
        /// @brief Whether the scaler discards its history this frame.
        bool Reset = false;
    };

    /// @brief A MetalFX temporal scaler: jittered sub-native colour accumulated into native.
    ///
    /// The scaler keeps its own history, so it needs nothing from the renderer but each frame's
    /// inputs. Released through the context's frame retire, so dropping it mid-frame is safe.
    class MetalFxTemporalScaler
    {
    public:
        /// @brief Creates a scaler of the given shape.
        /// @param context The render context; the scaler must not outlive it.
        /// @param info    The texture sizes and formats.
        /// @return The scaler, or null (with the reason logged) when MetalFX refuses the shape.
        static Unique<MetalFxTemporalScaler> Create(Context& context,
                                                    const MetalFxTemporalInfo& info);

        /// @brief Retires the scaler and its queue into the current frame's bin.
        ~MetalFxTemporalScaler();

        MetalFxTemporalScaler(const MetalFxTemporalScaler&) = delete;
        MetalFxTemporalScaler& operator=(const MetalFxTemporalScaler&) = delete;

        /// @brief Encodes one frame's reconstruction, splitting the frame around it.
        /// @param frame The frame's inputs and per-frame parameters.
        /// @pre Every image was created with ImageInfo::PlatformExport and matches the shape.
        void Encode(const MetalFxTemporalFrame& frame);

        /// @brief The shape the scaler was created with.
        [[nodiscard]] const MetalFxTemporalInfo& GetInfo() const { return m_Info; }

    private:
        MetalFxTemporalScaler(Context& context, const MetalFxTemporalInfo& info, void* scaler,
                              void* queue);

        Context& m_Context;
        /// @brief The shape the scaler was created with.
        MetalFxTemporalInfo m_Info;
        /// @brief The retained MTLFXTemporalScaler.
        void* m_Scaler = nullptr;
        /// @brief The retained MTLCommandQueue the scaler's work is committed to.
        void* m_Queue = nullptr;
    };

    /// @brief The fixed shape of a spatial scaler: its texture sizes and formats.
    struct MetalFxSpatialInfo
    {
        /// @brief The size of the colour input.
        uvec2 InputExtent = {1, 1};
        /// @brief The size of the output texture.
        uvec2 OutputExtent = {1, 1};
        /// @brief Format of the colour input, linear HDR.
        Format ColorFormat = Format::RGBA16Sfloat;
        /// @brief Format of the upscaled output.
        Format OutputFormat = Format::RGBA16Sfloat;
    };

    /// @brief One frame's encode of a spatial scaler.
    struct MetalFxSpatialFrame
    {
        /// @brief The colour, InputExtent-sized; its top-left ContentExtent is valid.
        const Image* Color = nullptr;
        /// @brief The OutputExtent-sized image the scaler writes in full.
        const Image* Output = nullptr;
        /// @brief The valid sub-rect of the colour this frame.
        uvec2 ContentExtent = {1, 1};
    };

    /// @brief A MetalFX spatial scaler: an edge-adaptive single-frame upscale of linear HDR colour.
    ///
    /// Released through the context's frame retire, so dropping it mid-frame is safe.
    class MetalFxSpatialScaler
    {
    public:
        /// @brief Creates a scaler of the given shape.
        /// @param context The render context; the scaler must not outlive it.
        /// @param info    The texture sizes and formats.
        /// @return The scaler, or null (with the reason logged) when MetalFX refuses the shape.
        static Unique<MetalFxSpatialScaler> Create(Context& context,
                                                   const MetalFxSpatialInfo& info);

        /// @brief Retires the scaler and its queue into the current frame's bin.
        ~MetalFxSpatialScaler();

        MetalFxSpatialScaler(const MetalFxSpatialScaler&) = delete;
        MetalFxSpatialScaler& operator=(const MetalFxSpatialScaler&) = delete;

        /// @brief Encodes one frame's upscale, splitting the frame around it.
        /// @param frame The frame's input, output and content size.
        /// @pre Both images were created with ImageInfo::PlatformExport and match the shape.
        void Encode(const MetalFxSpatialFrame& frame);

        /// @brief The shape the scaler was created with.
        [[nodiscard]] const MetalFxSpatialInfo& GetInfo() const { return m_Info; }

    private:
        MetalFxSpatialScaler(Context& context, const MetalFxSpatialInfo& info, void* scaler,
                             void* queue);

        Context& m_Context;
        /// @brief The shape the scaler was created with.
        MetalFxSpatialInfo m_Info;
        /// @brief The retained MTLFXSpatialScaler.
        void* m_Scaler = nullptr;
        /// @brief The retained MTLCommandQueue the scaler's work is committed to.
        void* m_Queue = nullptr;
    };

#ifndef __APPLE__
    inline MetalFxSupport QueryMetalFxSupport(const Context&)
    {
        return {};
    }

    inline Unique<MetalFxTemporalScaler> MetalFxTemporalScaler::Create(Context&,
                                                                       const MetalFxTemporalInfo&)
    {
        return nullptr;
    }

    inline MetalFxTemporalScaler::~MetalFxTemporalScaler() = default;

    inline void MetalFxTemporalScaler::Encode(const MetalFxTemporalFrame&) {}

    inline Unique<MetalFxSpatialScaler> MetalFxSpatialScaler::Create(Context&,
                                                                     const MetalFxSpatialInfo&)
    {
        return nullptr;
    }

    inline MetalFxSpatialScaler::~MetalFxSpatialScaler() = default;

    inline void MetalFxSpatialScaler::Encode(const MetalFxSpatialFrame&) {}
#endif
}
