#pragma once

#include <Veng/Vendor/ImGui.h>

#include <Veng/Veng.h>
#include <Veng/Renderer/Backend/CommandPool.h>
#include <Veng/Renderer/Backend/DescriptorPool.h>
#include <Veng/Renderer/Backend/ImGUITexture.h>
#include <Veng/Renderer/Backend/SynchronizationFrame.h>

#include <Veng/Renderer/Backend/SwapChain.h>
#include <Veng/Renderer/Backend/SwapChainSupport.h>
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
        WindowInfo WindowInfo;
        optional<path> DefaultFontPath;
        optional<path> IconFontPath;
    };

    class Context
    {
    public:
        Unique<Window> Initialize(const ContextInfo& info);
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
        vk::Format GetOutputFormat() { return vk::Format::eR16G16B16A16Sfloat; }
        vk::Format GetDepthFormat() { return vk::Format::eD32Sfloat; }

        static vk::PresentModeKHR GetPresentMode(
            const vector<vk::PresentModeKHR>& availablePresentModes);
        void UpdateRenderExtent();

        static Context& Instance();

        [[nodiscard]] uvec2 GetInternalRenderExtent() const { return m_InternalRenderExtent; }
        [[nodiscard]] uvec2 GetRenderExtent() const { return m_RenderExtent; }

        [[nodiscard]] u32 GetMaxFramesInFlight() const { return m_MaxFramesInFlight; }
        [[nodiscard]] u32 GetCurrentFrameInFlight() const { return m_CurrentFrameInFlight; }

        [[nodiscard]] SynchronizationFrame& GetCurrentSynchronizationFrame()
        {
            return m_SynchronizationFrames[m_CurrentFrameInFlight];
        }

        [[nodiscard]] Ref<Image> GetImGuiImage() const { return m_ImGuiImage; }

        void ImmediateCommands(const std::function<void(CommandBuffer&)>& function) const;
        void AcquireNextImage(Semaphore& semaphore);
        void WaitIdle() const;
        void RenderImGui(CommandBuffer& commandBuffer);
        void BeginFrame();
        Ref<ImGUITexture> CreateImGUITexture(const Sampler& sampler, const ImageView& imageView);
        void DestroyImGUITexture(const ImGUITexture& texture);

    private:
        static inline Context* s_Instance = nullptr;

        // Non-owning; the Window created in Initialize() is owned by the caller
        Window* m_Window = nullptr;

        uvec2 m_InternalRenderExtent;
        uvec2 m_RenderExtent;
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
        VkDebugUtilsMessengerEXT m_DebugMessenger = nullptr;
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
