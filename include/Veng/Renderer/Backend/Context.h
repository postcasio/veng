#pragma once

#include <Veng/Vendor/ImGui.h>

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/CommandPool.h>
#include <Veng/Renderer/Backend/DescriptorPool.h>
#include <Veng/Renderer/Backend/ImGuiTexture.h>
#include <Veng/Renderer/Backend/SynchronizationFrame.h>

#include <Veng/Renderer/Backend/SwapChain.h>
#include <Veng/Renderer/Backend/SwapChainSupport.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Window.h>

namespace Veng::Renderer
{
    struct QueueFamilyIndices
    {
        optional<u32> GraphicsFamily;
        optional<u32> PresentFamily;

        [[nodiscard]] bool IsComplete() const
        {
            return GraphicsFamily.has_value() && PresentFamily.has_value();
        }
    };

    struct ContextInfo
    {
        string ApplicationName;
        string EngineName = "Veng";
        uvec2 InternalRenderExtent;
        // Fonts live here until ImGui is extracted into its own module.
        optional<path> DefaultFontPath;
        optional<path> IconFontPath;
        Format OutputFormat = Format::RGBA16Sfloat;
        Format DepthFormat = Format::D32Sfloat;
    };

    class Context
    {
    public:
        // The window is borrowed, not owned; it must outlive the context and is
        // created by the application before the context initializes.
        void Initialize(const ContextInfo& info, Window* window);
        void DisposeResources();
        void Dispose();

        [[nodiscard]] const vk::Instance& GetVkInstance() const { return m_Instance; }
        [[nodiscard]] const vk::PhysicalDevice& GetVkPhysicalDevice() const { return m_PhysicalDevice; }
        [[nodiscard]] const vk::Device& GetVkDevice() const { return m_Device; }
        [[nodiscard]] const vk::Queue& GetVkGraphicsQueue() const { return m_GraphicsQueue; }
        [[nodiscard]] const vk::Queue& GetVkPresentQueue() const { return m_PresentQueue; }
        [[nodiscard]] const vk::SurfaceKHR& GetVkSurface() const { return m_Surface; }
        [[nodiscard]] const QueueFamilyIndices& GetQueueFamilies() const { return m_QueueFamilies; }
        [[nodiscard]] VmaAllocator GetAllocator() const { return m_Allocator; }
        [[nodiscard]] const SwapChain& GetSwapChain() const { return *m_SwapChain; }
        [[nodiscard]] const CommandPool& GetCommandPool() const { return *m_CommandPool; }
        [[nodiscard]] const DescriptorPool& GetDescriptorPool() const { return *m_DescriptorPool; }
        [[nodiscard]] Window& GetWindow() const { return *m_Window; }

        SynchronizationFrame& AcquireNextFrame();

        SynchronizationFrame& GetCurrentFrame()
        {
            return m_SynchronizationFrames[m_CurrentFrameInFlight];
        }

        void SubmitFrame(const SynchronizationFrame& frame) const;
        void PresentFrame(const SynchronizationFrame& frame);
        void SubmitImmediateCommands(CommandBuffer& commandBuffer) const;
        [[nodiscard]] Format GetOutputFormat() const { return m_OutputFormat; }
        [[nodiscard]] Format GetDepthFormat() const { return m_DepthFormat; }

        static vk::PresentModeKHR GetPresentMode(
            const vector<vk::PresentModeKHR>& availablePresentModes);
        void UpdateRenderExtent();

        static Context& Instance();

        [[nodiscard]] uvec2 GetInternalRenderExtent() const { return m_InternalRenderExtent; }
        [[nodiscard]] uvec2 GetRenderExtent() const { return m_RenderExtent; }

        [[nodiscard]] u32 GetMaxFramesInFlight() const { return m_MaxFramesInFlight; }
        [[nodiscard]] u32 GetCurrentFrameInFlight() const { return m_CurrentFrameInFlight; }

        [[nodiscard]] Ref<Image> GetImGuiImage() const { return m_ImGuiImage; }

        void ImmediateCommands(const std::function<void(CommandBuffer&)>& function) const;
        void AcquireNextImage(Semaphore& semaphore);
        void WaitIdle() const;
        void RenderImGui(CommandBuffer& commandBuffer);
        void BeginFrame();
        Ref<ImGuiTexture> CreateImGuiTexture(const Sampler& sampler, const ImageView& imageView);
        void DestroyImGuiTexture(const ImGuiTexture& texture);

        // Deferred destruction. Resource destructors hand their Vulkan handles
        // here instead of destroying them immediately; the handle is retired
        // into the current frame's bin and only destroyed once that frame's
        // fence has been waited again (see AcquireNextFrame). This makes
        // dropping the last Ref/Unique to a resource mid-frame always safe —
        // in-flight GPU work that still references the handle finishes first.
        void Retire(vk::Buffer buffer, VmaAllocation allocation);
        void Retire(vk::Image image, VmaAllocation allocation);
        void Retire(vk::ImageView imageView);
        void Retire(vk::Sampler sampler);
        void Retire(vk::ShaderModule shaderModule);
        void Retire(vk::Pipeline pipeline);
        void Retire(vk::PipelineLayout pipelineLayout);
        void Retire(vk::RenderPass renderPass);
        void Retire(vk::Framebuffer framebuffer);
        void Retire(vk::DescriptorSet descriptorSet);

    private:
        static inline Context* s_Instance = nullptr;

        // Borrowed from the application in Initialize(); never owned.
        Window* m_Window = nullptr;

        uvec2 m_InternalRenderExtent;
        uvec2 m_RenderExtent;
        Format m_OutputFormat = Format::RGBA16Sfloat;
        Format m_DepthFormat = Format::D32Sfloat;
        bool m_ImGuiRenderedThisFrame = true;

        vk::Instance m_Instance;
        vk::PhysicalDevice m_PhysicalDevice;
        vk::Device m_Device;
        vk::Queue m_GraphicsQueue;
        vk::Queue m_PresentQueue;
        vk::SurfaceKHR m_Surface;
        const vector<const char*> m_ValidationLayers = vector<const char*>({"VK_LAYER_KHRONOS_validation"});
        const vector<const char*> m_DeviceExtensions = vector<const char*>({
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,
            VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME,
            VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
            VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME
        });
        vector<const char*> m_RequiredExtensions;
        vk::DebugUtilsMessengerEXT m_DebugMessenger;
        QueueFamilyIndices m_QueueFamilies{};
        VmaAllocator m_Allocator = nullptr;
        Unique<SwapChain> m_SwapChain;
        Unique<CommandPool> m_CommandPool;
        Unique<DescriptorPool> m_DescriptorPool;
        Unique<DescriptorPool> m_ImGuiDescriptorPool;
        Ref<RenderPass> m_ImGuiRenderPass;
        vector<Ref<Framebuffer>> m_ImGuiFramebuffers;
        Ref<Image> m_ImGuiImage;
        Ref<ImageView> m_ImGuiImageView;

        vector<SynchronizationFrame> m_SynchronizationFrames{};
        u32 m_CurrentFrameInFlight = 0;
        u32 m_MaxFramesInFlight = 2;

        bool m_RenderExtentChanged = false;

        // One retire bin per in-flight frame. A handle retired while frame i is
        // recording goes into bin i and is protected by frame i's fence, so it
        // is destroyed when AcquireNextFrame next waits that fence. MaxFrames
        // bins suffice: retirements outside a frame (init, post-WaitIdle) are
        // caught by the shutdown drain-all in DisposeResources/Dispose.
        struct RetireBin
        {
            vector<std::pair<vk::Buffer, VmaAllocation>> Buffers;
            vector<std::pair<vk::Image, VmaAllocation>> Images;
            vector<vk::ImageView> ImageViews;
            vector<vk::Sampler> Samplers;
            vector<vk::ShaderModule> ShaderModules;
            vector<vk::Pipeline> Pipelines;
            vector<vk::PipelineLayout> PipelineLayouts;
            vector<vk::RenderPass> RenderPasses;
            vector<vk::Framebuffer> Framebuffers;
            vector<vk::DescriptorSet> DescriptorSets; // freed back to the descriptor pool
        };

        vector<RetireBin> m_RetireBins{};
        bool m_Disposed = false;

        RetireBin& CurrentRetireBin();
        void DrainRetireBin(RetireBin& bin);
        void DrainAllRetireBins();

        vector<const char*>& GetRequiredExtensions();
        vk::PhysicalDevice GetPhysicalDevice();
        bool IsDeviceSuitable(vk::PhysicalDevice device);
        QueueFamilyIndices& FindQueueFamilies(vk::PhysicalDevice device);
        [[nodiscard]] SwapChainSupportDetails QuerySwapChainSupport(vk::PhysicalDevice device) const;
        [[nodiscard]] bool CheckDeviceExtensionSupport(vk::PhysicalDevice device) const;
        vk::Device CreateDevice();
        void CreateImGuiResources();
        void DisposeImGuiResources();

        friend class SwapChain;
        friend class CommandBuffer;
    };
}
