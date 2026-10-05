#include "GamepadBackend.h"

#include <Veng/Log.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace Veng
{
    namespace
    {
        /// @brief How long one motor send runs before the pad stops on its own, in milliseconds.
        constexpr u32 MotorSendMs = 250;
        /// @brief How often a held non-zero level is re-sent, in milliseconds; under MotorSendMs, so
        ///        a running frame loop never lets the motors lapse.
        constexpr u64 MotorRenewMs = 100;

        /// @brief The SDL button behind each engine button, in GamepadButton order;
        ///        SDL_GAMEPAD_BUTTON_INVALID for TouchpadTouch, which is read from the touchpad.
        constexpr std::array<SDL_GamepadButton, usize(GamepadButton::Count)> ButtonToSdl{
            SDL_GAMEPAD_BUTTON_SOUTH,         SDL_GAMEPAD_BUTTON_EAST,
            SDL_GAMEPAD_BUTTON_WEST,          SDL_GAMEPAD_BUTTON_NORTH,
            SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
            SDL_GAMEPAD_BUTTON_BACK,          SDL_GAMEPAD_BUTTON_START,
            SDL_GAMEPAD_BUTTON_GUIDE,         SDL_GAMEPAD_BUTTON_LEFT_STICK,
            SDL_GAMEPAD_BUTTON_RIGHT_STICK,   SDL_GAMEPAD_BUTTON_DPAD_UP,
            SDL_GAMEPAD_BUTTON_DPAD_RIGHT,    SDL_GAMEPAD_BUTTON_DPAD_DOWN,
            SDL_GAMEPAD_BUTTON_DPAD_LEFT,     SDL_GAMEPAD_BUTTON_MISC1,
            SDL_GAMEPAD_BUTTON_TOUCHPAD,      SDL_GAMEPAD_BUTTON_INVALID,
            SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
            SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,  SDL_GAMEPAD_BUTTON_LEFT_PADDLE2};

        /// @brief The SDL axis behind each stick and trigger axis, in GamepadAxis order.
        constexpr std::array<SDL_GamepadAxis, 6> AxisToSdl{
            SDL_GAMEPAD_AXIS_LEFTX,  SDL_GAMEPAD_AXIS_LEFTY,        SDL_GAMEPAD_AXIS_RIGHTX,
            SDL_GAMEPAD_AXIS_RIGHTY, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER};

        static_assert(usize(GamepadAxis::RightTrigger) + 1 == AxisToSdl.size(),
                      "the stick and trigger axes precede the touchpad axes");

        /// @brief Maps SDL's pad family onto the engine's; a family the engine does not name is
        ///        reported as a standard pad.
        GamepadType ToGamepadType(const SDL_GamepadType type)
        {
            switch (type)
            {
            case SDL_GAMEPAD_TYPE_UNKNOWN:
                return GamepadType::Unknown;
            case SDL_GAMEPAD_TYPE_XBOX360:
                return GamepadType::Xbox360;
            case SDL_GAMEPAD_TYPE_XBOXONE:
                return GamepadType::XboxOne;
            case SDL_GAMEPAD_TYPE_PS3:
                return GamepadType::PS3;
            case SDL_GAMEPAD_TYPE_PS4:
                return GamepadType::PS4;
            case SDL_GAMEPAD_TYPE_PS5:
                return GamepadType::PS5;
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
                return GamepadType::SwitchPro;
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
                return GamepadType::JoyConLeft;
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
                return GamepadType::JoyConRight;
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
                return GamepadType::JoyConPair;
            default:
                return GamepadType::Standard;
            }
        }

        /// @brief Scales a 0..1 level onto SDL's 16-bit motor intensity.
        Uint16 ToIntensity(const f32 level)
        {
            return static_cast<Uint16>(std::lround(std::clamp(level, 0.0f, 1.0f) * 65535.0f));
        }

        /// @brief Reads one SDL pad's full state into a GamepadState.
        void ReadPad(SDL_Gamepad* pad, GamepadState& state)
        {
            state.Connected = true;
            for (usize button = 0; button < ButtonToSdl.size(); ++button)
            {
                state.Buttons[button] = ButtonToSdl[button] != SDL_GAMEPAD_BUTTON_INVALID &&
                                        SDL_GetGamepadButton(pad, ButtonToSdl[button]);
            }

            for (usize axis = 0; axis < AxisToSdl.size(); ++axis)
            {
                const f32 raw =
                    static_cast<f32>(SDL_GetGamepadAxis(pad, AxisToSdl[axis])) / 32767.0f;
                const bool trigger = AxisToSdl[axis] == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                                     AxisToSdl[axis] == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                state.Axes[axis] = std::clamp(raw, trigger ? 0.0f : -1.0f, 1.0f);
            }

            bool down = false;
            f32 x = 0.0f;
            f32 y = 0.0f;
            if (SDL_GetNumGamepadTouchpads(pad) > 0)
            {
                f32 pressure = 0.0f;
                if (!SDL_GetGamepadTouchpadFinger(pad, 0, 0, &down, &x, &y, &pressure))
                {
                    down = false;
                }
            }
            state.Buttons[static_cast<usize>(GamepadButton::TouchpadTouch)] = down;
            state.Axes[static_cast<usize>(GamepadAxis::TouchpadX)] = down ? x : 0.0f;
            state.Axes[static_cast<usize>(GamepadAxis::TouchpadY)] = down ? y : 0.0f;

            state.Type = ToGamepadType(SDL_GetGamepadType(pad));
            const char* name = SDL_GetGamepadName(pad);
            state.Name.assign(name != nullptr ? name : "");
        }
    }

    struct GamepadBackend::Native
    {
        /// @brief One open physical pad.
        struct Device
        {
            /// @brief The open SDL pad.
            SDL_Gamepad* Pad = nullptr;
            /// @brief Whether the pad has grip rumble motors.
            bool Rumble = false;
            /// @brief Whether the pad has trigger motors.
            bool TriggerMotors = false;
            /// @brief The levels last sent to the pad.
            GamepadMotors Sent;
            /// @brief SDL's tick count at the last send, in milliseconds.
            u64 SentAtMs = 0;
        };

        /// @brief Whether SDL's gamepad subsystem came up.
        bool Initialized = false;
        /// @brief The open pads, keyed by SDL instance id.
        unordered_map<u64, Device> Devices;
    };

    GamepadBackend::GamepadBackend() : m_Native(CreateUnique<Native>())
    {
        // SDL's own handlers would turn SIGINT/SIGTERM into a quit event nothing here reads, so a
        // kill by PID or Ctrl+C would stop ending the process. Override priority, so the environment
        // cannot put them back.
        SDL_SetHintWithPriority(SDL_HINT_NO_SIGNAL_HANDLERS, "1", SDL_HINT_OVERRIDE);
#ifdef _WIN32
        // Detection and raw input on SDL's thread, independent of GLFW's message pump; WGI off, which
        // removes its duplicate race with raw input and its COM dependency.
        SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_THREAD, "1", SDL_HINT_OVERRIDE);
        SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_WGI, "0", SDL_HINT_OVERRIDE);
#endif

        if (!SDL_Init(SDL_INIT_GAMEPAD))
        {
            Log::Warn("Gamepads unavailable: SDL could not start its gamepad subsystem ({})",
                      SDL_GetError());
            return;
        }
        m_Native->Initialized = true;
    }

    GamepadBackend::~GamepadBackend()
    {
        if (!m_Native->Initialized)
        {
            return;
        }
        for (auto& [id, device] : m_Native->Devices)
        {
            SDL_CloseGamepad(device.Pad);
        }
        m_Native->Devices.clear();
        SDL_Quit();
    }

    void GamepadBackend::Update(const std::span<GamepadState> states, const bool focused)
    {
        if (m_Native->Initialized)
        {
            // A pad present at startup arrives as an added event too, so the queue is the one
            // source of connects.
            SDL_UpdateGamepads();
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_EVENT_GAMEPAD_ADDED)
                {
                    const SDL_JoystickID id = event.gdevice.which;
                    if (m_Native->Devices.contains(id))
                    {
                        continue;
                    }
                    SDL_Gamepad* pad = SDL_OpenGamepad(id);
                    if (pad == nullptr)
                    {
                        Log::Warn("Gamepad {} could not be opened: {}", id, SDL_GetError());
                        continue;
                    }
                    const SDL_PropertiesID properties = SDL_GetGamepadProperties(pad);
                    m_Native->Devices.emplace(
                        id, Native::Device{
                                .Pad = pad,
                                .Rumble = SDL_GetBooleanProperty(
                                    properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false),
                                .TriggerMotors = SDL_GetBooleanProperty(
                                    properties, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false),
                            });
                    m_Slots.Attach(id);
                    const char* name = SDL_GetGamepadName(pad);
                    Log::Info("Gamepad connected: {} ({})", name != nullptr ? name : "unnamed",
                              SDL_GetGamepadStringForType(SDL_GetGamepadType(pad)));
                }
                else if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
                {
                    const SDL_JoystickID id = event.gdevice.which;
                    const auto found = m_Native->Devices.find(id);
                    if (found == m_Native->Devices.end())
                    {
                        continue;
                    }
                    SDL_CloseGamepad(found->second.Pad);
                    m_Native->Devices.erase(found);
                    m_Slots.Detach(id);
                    Log::Info("Gamepad {} disconnected", id);
                }
            }
        }

        m_Slots.Commit();
        m_Slots.Fill(states, focused,
                     [this](const u64 device, GamepadState& state)
                     {
                         const auto found = m_Native->Devices.find(device);
                         if (found != m_Native->Devices.end())
                         {
                             ReadPad(found->second.Pad, state);
                         }
                     });
        FlushMotors();
    }

    void GamepadBackend::FlushMotors()
    {
        if (!m_Native->Initialized)
        {
            return;
        }
        const u64 now = SDL_GetTicks();
        for (usize index = 0; index < GamepadSlots::SlotCount; ++index)
        {
            const auto slot = static_cast<GamepadId>(index);
            const optional<u64> key = m_Slots.GetDevice(slot);
            if (!key)
            {
                continue;
            }
            const auto found = m_Native->Devices.find(*key);
            if (found == m_Native->Devices.end())
            {
                continue;
            }
            Native::Device& device = found->second;

            const GamepadMotors wanted = m_Slots.GetMotors(slot);
            const bool active = wanted != GamepadMotors{};
            const bool due = active && now - device.SentAtMs >= MotorRenewMs;
            if (wanted == device.Sent && !due)
            {
                continue;
            }

            const u32 duration = active ? MotorSendMs : 0;
            if (device.Rumble)
            {
                SDL_RumbleGamepad(device.Pad, ToIntensity(wanted.Low), ToIntensity(wanted.High),
                                  duration);
            }
            if (device.TriggerMotors)
            {
                SDL_RumbleGamepadTriggers(device.Pad, ToIntensity(wanted.LeftTrigger),
                                          ToIntensity(wanted.RightTrigger), duration);
            }
            device.Sent = wanted;
            device.SentAtMs = now;
        }
    }

    vector<Unique<Event>> GamepadBackend::TakeEvents()
    {
        return m_Slots.TakeEvents();
    }

    void GamepadBackend::ApplyVirtual(const VirtualGamepadEvent& edit)
    {
        m_Slots.ApplyVirtual(edit);
    }

    void GamepadBackend::SetMotors(const GamepadId slot, const GamepadMotors& motors)
    {
        m_Slots.SetMotors(slot, motors);
    }

    GamepadMotors GamepadBackend::GetMotors(const GamepadId slot) const
    {
        return m_Slots.GetMotors(slot);
    }

    bool GamepadBackend::HasRumble(const GamepadId slot) const
    {
        const optional<u64> key = m_Slots.GetDevice(slot);
        const auto found = key ? m_Native->Devices.find(*key) : m_Native->Devices.end();
        return found != m_Native->Devices.end() && found->second.Rumble;
    }

    bool GamepadBackend::HasTriggerMotors(const GamepadId slot) const
    {
        const optional<u64> key = m_Slots.GetDevice(slot);
        const auto found = key ? m_Native->Devices.find(*key) : m_Native->Devices.end();
        return found != m_Native->Devices.end() && found->second.TriggerMotors;
    }
}
