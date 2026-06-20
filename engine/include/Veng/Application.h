#pragma once

#include <Veng/Veng.h>
#include <Veng/Window.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/Context.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Task/TaskSystem.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    /// @brief Returns the directory containing the running executable.
    ///
    /// A game mounts its asset pack relative to this so the launcher + module +
    /// pack resolve beside the binary, not at an absolute build-tree path.
    /// @return Absolute path to the directory holding the running binary.
    [[nodiscard]] VE_API path ExecutableDirectory();

    /// @brief Construction parameters for Application.
    struct ApplicationInfo
    {
        /// @brief Application name passed to the Vulkan instance.
        string Name = "Veng Application";
        /// @brief Engine name passed to the Vulkan instance.
        string EngineName = "Veng";
        /// @brief Off-screen render resolution before presentation.
        uvec2 InternalRenderExtent{1280, 720};
        /// @brief Window creation parameters.
        WindowInfo WindowInfo;
        /// @brief ImGui integration; nullopt disables it for UI-free apps.
        ///
        /// Engaged by default. Force-disabled when Headless (ImGui requires a window).
        optional<ImGuiLayerInfo> ImGui = ImGuiLayerInfo{};
        /// @brief Run without a window, using an off-screen context; exits on RequestExit().
        bool Headless = false;
        /// @brief Path for pipeline cache persistence; nullopt keeps the cache in-memory only.
        ///
        /// When set, seeds the pipeline cache from this file at startup (if it exists)
        /// and writes it back at shutdown. veng does not choose the path.
        optional<path> PipelineCachePath = std::nullopt;
    };

    /// @brief Base class for a veng application; subclass and override the lifecycle hooks.
    class Application
    {
    public:
        /// @brief Constructs the application with the given settings and a borrowed type registry.
        ///
        /// The TypeRegistry is borrowed, not owned: the host (launcher or cooker)
        /// constructs it and pre-registers builtins before VengModuleRegister runs.
        /// It must outlive this Application.
        /// @param info   Application creation parameters.
        /// @param types  Host-owned registry of reflected types; must outlive the app.
        Application(ApplicationInfo info, TypeRegistry& types);
        virtual ~Application() = default;

        /// @brief Enter the main loop, blocking until the app exits.
        /// @param arguments  Command-line arguments forwarded from the launcher.
        void Run(vector<string> arguments);

        /// @brief Returns the application window.
        [[nodiscard]] Window& GetWindow() const
        {
            return *m_Window;
        }

        /// @brief Returns the render context.
        [[nodiscard]] Renderer::Context& GetRenderContext()
        {
            return m_RenderContext;
        }

        /// @brief Returns the task system.
        [[nodiscard]] TaskSystem& GetTaskSystem()
        {
            return *m_TaskSystem;
        }

        /// @brief Returns the asset manager.
        [[nodiscard]] AssetManager& GetAssetManager()
        {
            return *m_AssetManager;
        }

        /// @brief Returns the host-owned, process-wide registry of reflected types.
        ///
        /// Borrowed: the host constructs it, pre-registers builtins, and calls
        /// VengModuleRegister before passing it here. Must outlive this Application.
        [[nodiscard]] TypeRegistry& GetTypeRegistry()
        {
            return m_TypeRegistry;
        }

        /// @brief Returns the ImGui layer, or nullptr if the app opted out.
        [[nodiscard]] ImGuiLayer* GetImGuiLayer() const
        {
            return m_ImGuiLayer.get();
        }

    protected:
        /// @brief Called once after all engine systems are initialized.
        virtual void OnInitialize()
        {
        }

        /// @brief Called once per frame before rendering.
        /// @param delta  Time in seconds since the previous frame.
        virtual void OnUpdate(f32 delta)
        {
        }

        /// @brief Called once per frame to record draw commands.
        virtual void OnRender()
        {
        }

        /// @brief Called after the main loop exits and the GPU is idle, before context teardown.
        ///
        /// Release every engine resource held by the application here (reset Refs/Uniques,
        /// AssetHandles included) — resources that outlive the context fail on destruction.
        virtual void OnDispose()
        {
        }

        /// @brief Signals the run loop to exit after the current frame.
        ///
        /// The only way to stop a headless app; also works for windowed apps.
        void RequestExit() { m_ShouldExit = true; }

    private:
        void Initialize();
        void Frame();

        ApplicationInfo m_Info;

        /// @brief Borrowed from the host; must outlive this app and every Scene it creates.
        TypeRegistry& m_TypeRegistry;

        Unique<Window> m_Window;

        Renderer::Context m_RenderContext;

        /// @brief Worker pool; destroyed after m_AssetManager to avoid tearing down live workers.
        Unique<TaskSystem> m_TaskSystem;

        /// @brief Constructed after m_RenderContext; reset before teardown so assets retire safely.
        Unique<AssetManager> m_AssetManager;

        Unique<ImGuiLayer> m_ImGuiLayer;

        bool m_ShouldExit = false;
    };
}
