#pragma once

#include <Veng/Veng.h>
#include <Veng/Input.h>
#include <Veng/WorldRunner.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Haptics/Haptics.h>
#include <Veng/Localization/Localization.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng::TestSupport
{
    /// @brief A case's own services, read by a TestServices bundle in place of its built-in ones.
    struct TestServicesInfo
    {
        /// @brief An asset manager holding the case's mounted packs; null reads the bundle's own.
        AssetManager* Assets = nullptr;
        /// @brief An input snapshot the case drives with events; null reads the headless one.
        const Veng::Input* Input = nullptr;
        /// @brief An audio engine the case inspects; null plays through the bundle's null device.
        Audio::AudioEngine* Audio = nullptr;
    };

    /// @brief Real, device-free instances of every service a SystemContext carries, for tests.
    ///
    /// A system under test that touches a service reaches a working one: an asset manager over an
    /// uninitialized render context and a one-worker task system, a headless input snapshot (all
    /// zeros), an audio engine over the null device, a haptics engine driving no pads, and the inert
    /// localization that resolves every key to itself. Construct one per case; it is not copyable.
    class TestServices
    {
    public:
        /// @brief Builds every service.
        TestServices() : TestServices(TestServicesInfo{}) {}

        /// @brief Builds every service, reading the case's own where @p info names one.
        /// @param info  The case's own services the contexts read instead.
        explicit TestServices(const TestServicesInfo& info)
            : m_Assets(m_Context, m_Tasks, m_Types),
              m_Audio(Audio::AudioDevice::Create(Audio::AudioDeviceInfo{
                  .Backend = Audio::AudioBackend::Null, .SampleRate = 48000, .Channels = 2})),
              m_AssetsOverride(info.Assets), m_InputOverride(info.Input),
              m_AudioOverride(info.Audio)
        {
        }

        TestServices(const TestServices&) = delete;
        TestServices& operator=(const TestServices&) = delete;

        /// @brief Returns a context over the services with every per-call field at its default.
        [[nodiscard]] SystemContext Make()
        {
            return SystemContext{
                .Assets = m_AssetsOverride != nullptr ? *m_AssetsOverride : m_Assets,
                .Input = m_InputOverride != nullptr ? *m_InputOverride : m_Input,
                .Tasks = m_Tasks,
                .Audio = m_AudioOverride != nullptr ? *m_AudioOverride : m_Audio->GetEngine(),
                .Haptics = m_Haptics,
                .Localization = m_Localization,
            };
        }

        /// @brief Returns the context @p request describes, as a WorldRunner's factory builds it.
        ///
        /// Stamps the world, tick, alpha, step edges and the replay flag from the request; the role
        /// stays Server, and Pointer, View and Debug stay empty (a test presents nothing).
        /// @param request  The world, scene, phase and step the context is for.
        /// @return The context.
        [[nodiscard]] SystemContext Make(const SystemContextRequest& request)
        {
            SystemContext context = Make();
            context.World = request.World;
            context.Tick = request.Tick;
            context.Alpha = request.Alpha;
            context.FirstStepThisFrame = request.FirstStep;
            context.LastStepThisFrame = request.LastStep;
            context.IsReplay = request.Phase == SystemContextPhase::Replay;
            return context;
        }

        /// @brief Returns a context factory over these services, to install on a test's runner.
        /// @return A factory calling Make(request); it borrows this bundle, which must outlive it.
        [[nodiscard]] SystemContextFactory Factory()
        {
            return [this](const SystemContextRequest& request) { return Make(request); };
        }

        /// @brief Returns the asset manager.
        [[nodiscard]] AssetManager& GetAssets() { return m_Assets; }

        /// @brief Returns the bundle's own headless input, for a case to feed events through.
        [[nodiscard]] Input& GetInput() { return m_Input; }

        /// @brief Returns the audio engine over the null device.
        [[nodiscard]] Audio::AudioEngine& GetAudio() { return m_Audio->GetEngine(); }

        /// @brief Returns the inert localization service.
        [[nodiscard]] Localization::Localization& GetLocalization() { return m_Localization; }

    private:
        /// @brief The render context the asset manager is bound to; never initialized.
        Renderer::Context m_Context;
        /// @brief The task system the asset manager and the context's Tasks run on.
        TaskSystem m_Tasks{TaskSystemInfo{.WorkerCount = 1}};
        /// @brief The type registry the asset manager resolves through.
        TypeRegistry m_Types;
        /// @brief The asset manager.
        AssetManager m_Assets;
        /// @brief The headless input snapshot.
        Input m_Input{nullptr};
        /// @brief The null audio device whose engine the context's Audio is.
        Unique<Audio::AudioDevice> m_Audio;
        /// @brief The haptics engine, driving no pads.
        Haptics::HapticsEngine m_Haptics;
        /// @brief The inert localization service.
        Localization::Localization m_Localization;
        /// @brief The case's own asset manager, read in place of m_Assets; null for none.
        AssetManager* m_AssetsOverride = nullptr;
        /// @brief The case's own input, read in place of m_Input; null for none.
        const Input* m_InputOverride = nullptr;
        /// @brief The case's own audio engine, read in place of m_Audio's; null for none.
        Audio::AudioEngine* m_AudioOverride = nullptr;
    };

    /// @brief Returns a bundle that lives for the whole process, for a helper outside any case.
    ///
    /// Deliberately never destroyed, so no static-destruction order can reach its worker thread or
    /// audio device after the services they touch are gone.
    inline TestServices& SharedTestServices()
    {
        static auto* const services = new TestServices();
        return *services;
    }
}
