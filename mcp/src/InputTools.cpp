#include "InputTools.h"

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Input.h>
#include <Veng/InputEvents.h>

#include <nlohmann/json.hpp>

#include <fmt/format.h>

#include <array>
#include <utility>

namespace Veng::Mcp
{
    using Json = nlohmann::json;

    namespace
    {
        /// @brief The most input events one input.send call applies.
        ///
        /// Mirrors the mutation batch cap: a context-volume convention for a single trusted local
        /// client, not a DoS defense. A batch over this size is a whole-call located error.
        constexpr usize MaxInputBatchSize = 64;

        /// @brief The most codepoints one 'text' event carries.
        ///
        /// A text event expands to one KeyTypedEvent per codepoint, so this bounds the fan-out a
        /// single batch entry produces — the same context-volume convention as the batch cap.
        constexpr usize MaxTextCodepoints = 256;

        /// @brief A name → Key row, so an agent names a key ("W", "Space") rather than a code.
        struct KeyName
        {
            /// @brief The name an agent passes.
            const char* Name;
            /// @brief The key it resolves to.
            Key Value;
        };

        /// @brief The named keys input.send accepts, matching the Key enumerators.
        constexpr std::array KeyNames{KeyName{"Space", Key::Space},
                                      KeyName{"Apostrophe", Key::Apostrophe},
                                      KeyName{"Comma", Key::Comma},
                                      KeyName{"Minus", Key::Minus},
                                      KeyName{"Period", Key::Period},
                                      KeyName{"Slash", Key::Slash},
                                      KeyName{"Num0", Key::Num0},
                                      KeyName{"Num1", Key::Num1},
                                      KeyName{"Num2", Key::Num2},
                                      KeyName{"Num3", Key::Num3},
                                      KeyName{"Num4", Key::Num4},
                                      KeyName{"Num5", Key::Num5},
                                      KeyName{"Num6", Key::Num6},
                                      KeyName{"Num7", Key::Num7},
                                      KeyName{"Num8", Key::Num8},
                                      KeyName{"Num9", Key::Num9},
                                      KeyName{"Semicolon", Key::Semicolon},
                                      KeyName{"Equal", Key::Equal},
                                      KeyName{"A", Key::A},
                                      KeyName{"B", Key::B},
                                      KeyName{"C", Key::C},
                                      KeyName{"D", Key::D},
                                      KeyName{"E", Key::E},
                                      KeyName{"F", Key::F},
                                      KeyName{"G", Key::G},
                                      KeyName{"H", Key::H},
                                      KeyName{"I", Key::I},
                                      KeyName{"J", Key::J},
                                      KeyName{"K", Key::K},
                                      KeyName{"L", Key::L},
                                      KeyName{"M", Key::M},
                                      KeyName{"N", Key::N},
                                      KeyName{"O", Key::O},
                                      KeyName{"P", Key::P},
                                      KeyName{"Q", Key::Q},
                                      KeyName{"R", Key::R},
                                      KeyName{"S", Key::S},
                                      KeyName{"T", Key::T},
                                      KeyName{"U", Key::U},
                                      KeyName{"V", Key::V},
                                      KeyName{"W", Key::W},
                                      KeyName{"X", Key::X},
                                      KeyName{"Y", Key::Y},
                                      KeyName{"Z", Key::Z},
                                      KeyName{"LeftBracket", Key::LeftBracket},
                                      KeyName{"Backslash", Key::Backslash},
                                      KeyName{"RightBracket", Key::RightBracket},
                                      KeyName{"GraveAccent", Key::GraveAccent},
                                      KeyName{"Escape", Key::Escape},
                                      KeyName{"Enter", Key::Enter},
                                      KeyName{"Tab", Key::Tab},
                                      KeyName{"Backspace", Key::Backspace},
                                      KeyName{"Insert", Key::Insert},
                                      KeyName{"Delete", Key::Delete},
                                      KeyName{"Right", Key::Right},
                                      KeyName{"Left", Key::Left},
                                      KeyName{"Down", Key::Down},
                                      KeyName{"Up", Key::Up},
                                      KeyName{"PageUp", Key::PageUp},
                                      KeyName{"PageDown", Key::PageDown},
                                      KeyName{"Home", Key::Home},
                                      KeyName{"End", Key::End},
                                      KeyName{"F1", Key::F1},
                                      KeyName{"F2", Key::F2},
                                      KeyName{"F3", Key::F3},
                                      KeyName{"F4", Key::F4},
                                      KeyName{"F5", Key::F5},
                                      KeyName{"F6", Key::F6},
                                      KeyName{"F7", Key::F7},
                                      KeyName{"F8", Key::F8},
                                      KeyName{"F9", Key::F9},
                                      KeyName{"F10", Key::F10},
                                      KeyName{"F11", Key::F11},
                                      KeyName{"F12", Key::F12},
                                      KeyName{"LeftShift", Key::LeftShift},
                                      KeyName{"LeftControl", Key::LeftControl},
                                      KeyName{"LeftAlt", Key::LeftAlt},
                                      KeyName{"LeftSuper", Key::LeftSuper},
                                      KeyName{"RightShift", Key::RightShift},
                                      KeyName{"RightControl", Key::RightControl},
                                      KeyName{"RightAlt", Key::RightAlt},
                                      KeyName{"RightSuper", Key::RightSuper}};

        /// @brief Resolves a key name to its Key, or nullopt when no enumerator matches.
        optional<Key> ResolveKey(string_view name)
        {
            for (const KeyName& entry : KeyNames)
            {
                if (name == entry.Name)
                {
                    return entry.Value;
                }
            }
            return std::nullopt;
        }

        /// @brief Resolves a mouse-button name (Left/Right/Middle) to its MouseButton.
        optional<MouseButton> ResolveMouseButton(string_view name)
        {
            if (name == "Left")
            {
                return MouseButton::Left;
            }
            if (name == "Right")
            {
                return MouseButton::Right;
            }
            if (name == "Middle")
            {
                return MouseButton::Middle;
            }
            return std::nullopt;
        }

        /// @brief Decodes a UTF-8 run to codepoints, or nullopt when the encoding is malformed.
        ///
        /// A text event names what a user types, so the argument is ordinary UTF-8 rather than a
        /// codepoint array; rejecting a malformed run up front keeps the batch's validate-then-apply
        /// discipline (a bad run fails the whole call, never half-types).
        optional<vector<u32>> DecodeUtf8(string_view text)
        {
            vector<u32> codepoints;
            for (usize i = 0; i < text.size();)
            {
                const auto lead = static_cast<u8>(text[i]);
                usize length = 0;
                u32 codepoint = 0;
                if (lead < 0x80)
                {
                    length = 1;
                    codepoint = lead;
                }
                else if ((lead & 0xE0) == 0xC0)
                {
                    length = 2;
                    codepoint = lead & 0x1Fu;
                }
                else if ((lead & 0xF0) == 0xE0)
                {
                    length = 3;
                    codepoint = lead & 0x0Fu;
                }
                else if ((lead & 0xF8) == 0xF0)
                {
                    length = 4;
                    codepoint = lead & 0x07u;
                }
                else
                {
                    return std::nullopt;
                }

                if (i + length > text.size())
                {
                    return std::nullopt;
                }
                for (usize k = 1; k < length; ++k)
                {
                    const auto continuation = static_cast<u8>(text[i + k]);
                    if ((continuation & 0xC0) != 0x80)
                    {
                        return std::nullopt;
                    }
                    codepoint = (codepoint << 6) | (continuation & 0x3Fu);
                }
                codepoints.push_back(codepoint);
                i += length;
            }
            return codepoints;
        }

        /// @brief Reads a required numeric field as f32, or nullopt when absent/not a number.
        optional<f32> ReadNumber(const Json& event, const char* field)
        {
            if (!event.contains(field) || !event[field].is_number())
            {
                return std::nullopt;
            }
            return event[field].get<f32>();
        }

        /// @brief One validated, resolved input event ready to apply on the render thread.
        ///
        /// Kept as a plain tagged struct so the whole batch is validated up front and only then
        /// applied — a structural error in any event rejects the whole call before a single event
        /// lands, matching the mutation batch verbs' validate-then-apply discipline.
        struct ResolvedEvent
        {
            /// @brief Which event kind this is; selects which fields below are meaningful.
            enum class Kind : u8
            {
                KeyDown,
                KeyUp,
                KeyRepeat,
                MouseDown,
                MouseUp,
                MouseMove,
                Scroll,
                Text,
            } Which = Kind::KeyDown;

            /// @brief The key for KeyDown/KeyUp/KeyRepeat.
            Key KeyCode = Key::Space;
            /// @brief The button for MouseDown/MouseUp.
            MouseButton Button = MouseButton::Left;
            /// @brief The position (MouseMove) or scroll offset (Scroll), in window pixels.
            vec2 Vector = {};
            /// @brief The decoded codepoints for Text, applied one KeyTypedEvent each in order.
            vector<u32> Codepoints;
        };

        /// @brief Validates and resolves one event object, or returns a located error naming its index.
        Result<ResolvedEvent> ResolveOne(const Json& event, usize index)
        {
            if (!event.is_object() || !event.contains("type") || !event["type"].is_string())
            {
                return std::unexpected(
                    fmt::format("events[{}] must be an object with a string 'type'", index));
            }
            const string type = event["type"].get<string>();

            const auto keyEvent = [&](ResolvedEvent::Kind kind) -> Result<ResolvedEvent>
            {
                if (!event.contains("key") || !event["key"].is_string())
                {
                    return std::unexpected(
                        fmt::format("events[{}] '{}' needs a string 'key'", index, type));
                }
                const optional<Key> key = ResolveKey(event["key"].get<string>());
                if (!key)
                {
                    return std::unexpected(fmt::format("events[{}] unknown key '{}'", index,
                                                       event["key"].get<string>()));
                }
                return ResolvedEvent{.Which = kind, .KeyCode = *key};
            };

            const auto buttonEvent = [&](ResolvedEvent::Kind kind) -> Result<ResolvedEvent>
            {
                if (!event.contains("button") || !event["button"].is_string())
                {
                    return std::unexpected(
                        fmt::format("events[{}] '{}' needs a string 'button'", index, type));
                }
                const optional<MouseButton> button =
                    ResolveMouseButton(event["button"].get<string>());
                if (!button)
                {
                    return std::unexpected(fmt::format("events[{}] unknown button '{}'", index,
                                                       event["button"].get<string>()));
                }
                return ResolvedEvent{.Which = kind, .Button = *button};
            };

            const auto vectorEvent = [&](ResolvedEvent::Kind kind, const char* fx,
                                         const char* fy) -> Result<ResolvedEvent>
            {
                const optional<f32> x = ReadNumber(event, fx);
                const optional<f32> y = ReadNumber(event, fy);
                if (!x || !y)
                {
                    return std::unexpected(fmt::format(
                        "events[{}] '{}' needs numbers '{}' and '{}'", index, type, fx, fy));
                }
                return ResolvedEvent{.Which = kind, .Vector = vec2{*x, *y}};
            };

            if (type == "key_down")
            {
                return keyEvent(ResolvedEvent::Kind::KeyDown);
            }
            if (type == "key_up")
            {
                return keyEvent(ResolvedEvent::Kind::KeyUp);
            }
            if (type == "key_repeat")
            {
                return keyEvent(ResolvedEvent::Kind::KeyRepeat);
            }
            if (type == "mouse_down")
            {
                return buttonEvent(ResolvedEvent::Kind::MouseDown);
            }
            if (type == "mouse_up")
            {
                return buttonEvent(ResolvedEvent::Kind::MouseUp);
            }
            if (type == "mouse_move")
            {
                return vectorEvent(ResolvedEvent::Kind::MouseMove, "x", "y");
            }
            if (type == "scroll")
            {
                return vectorEvent(ResolvedEvent::Kind::Scroll, "dx", "dy");
            }
            if (type == "text")
            {
                if (!event.contains("text") || !event["text"].is_string())
                {
                    return std::unexpected(
                        fmt::format("events[{}] 'text' needs a string 'text'", index));
                }
                const string run = event["text"].get<string>();
                optional<vector<u32>> codepoints = DecodeUtf8(run);
                if (!codepoints)
                {
                    return std::unexpected(
                        fmt::format("events[{}] 'text' is not valid UTF-8", index));
                }
                if (codepoints->empty())
                {
                    return std::unexpected(
                        fmt::format("events[{}] 'text' must name at least one character", index));
                }
                if (codepoints->size() > MaxTextCodepoints)
                {
                    return std::unexpected(
                        fmt::format("events[{}] 'text' exceeds the limit of {} characters", index,
                                    MaxTextCodepoints));
                }
                return ResolvedEvent{.Which = ResolvedEvent::Kind::Text,
                                     .Codepoints = std::move(*codepoints)};
            }
            return std::unexpected(fmt::format("events[{}] unknown type '{}'", index, type));
        }

        /// @brief Applies one resolved event through the host's injection seam.
        ///
        /// Builds the concrete Veng::Event and hands it to InjectInput, so it folds into the app's
        /// input exactly as a real window event does. Scancode/mods are zero — a synthetic event
        /// carries no platform scancode, which only the ImGui text sink reads.
        void ApplyResolved(const McpHost& host, const ResolvedEvent& resolved)
        {
            switch (resolved.Which)
            {
            case ResolvedEvent::Kind::KeyDown:
            {
                KeyPressedEvent event(resolved.KeyCode, 0, 0);
                host.InjectInput(event);
                break;
            }
            case ResolvedEvent::Kind::KeyUp:
            {
                KeyReleasedEvent event(resolved.KeyCode, 0, 0);
                host.InjectInput(event);
                break;
            }
            case ResolvedEvent::Kind::KeyRepeat:
            {
                KeyRepeatEvent event(resolved.KeyCode, 0, 0);
                host.InjectInput(event);
                break;
            }
            case ResolvedEvent::Kind::MouseDown:
            {
                MouseButtonPressedEvent event(resolved.Button, 0);
                host.InjectInput(event);
                break;
            }
            case ResolvedEvent::Kind::MouseUp:
            {
                MouseButtonReleasedEvent event(resolved.Button, 0);
                host.InjectInput(event);
                break;
            }
            case ResolvedEvent::Kind::MouseMove:
            {
                MouseMovedEvent event(resolved.Vector);
                host.InjectInput(event);
                break;
            }
            case ResolvedEvent::Kind::Scroll:
            {
                MouseScrolledEvent event(resolved.Vector);
                host.InjectInput(event);
                break;
            }
            case ResolvedEvent::Kind::Text:
            {
                // Text entry rides KeyTypedEvent — the same event the platform layer raises from a
                // character callback and the sole path a focused text field reads — so a driven run
                // lands through exactly the code a typing user does.
                for (const u32 codepoint : resolved.Codepoints)
                {
                    KeyTypedEvent event(codepoint);
                    host.InjectInput(event);
                }
                break;
            }
            }
        }
    }

    void RegisterInputTools(McpServer& server, const McpHost& host)
    {
        // input.send — an ordered batch of synthetic input events fed into the running app.
        {
            McpTool tool;
            tool.Name = "input.send";
            tool.Description =
                "Sends an ordered batch of synthetic input events to drive the running app, as "
                "if produced by the keyboard/mouse. Argument: { events: [ ... ] }, each event one "
                "of: { type: 'key_down'|'key_up'|'key_repeat', key: <name> }, { type: "
                "'mouse_down'|'mouse_up', button: 'Left'|'Right'|'Middle' }, { type: "
                "'mouse_move', x: <px>, y: <px> } (window-space position), { type: 'scroll', dx: "
                "<n>, dy: <n> }, { type: 'text', text: <utf8 string> }. Key names match the engine "
                "Key enumerators (e.g. 'W', 'Space', 'LeftShift', 'F1'). A 'text' event types its "
                "characters into whatever holds text focus, one character event per codepoint, the "
                "same path a keyboard's character callback drives — that is how to fill a text "
                "field; a key_down does not produce characters. A 'key_repeat' event is the "
                "platform auto-repeat of a key already held: send one per repetition to drive a "
                "repeating action such as caret movement or deletion in a focused text field. It "
                "never re-arms a one-shot press query, so it cannot fire a discrete action twice. "
                "Up to 256 characters per text event. Events apply in order at the frame's input "
                "point, so the action layer "
                "resolves them as real input. Up to 64 events per call.";
            tool.InputSchemaJson =
                R"({"type":"object","required":["events"],"properties":{"events":{"type":"array",)"
                R"("items":{"type":"object","required":["type"],"properties":{)"
                R"("type":{"type":"string","enum":["key_down","key_up","key_repeat",)"
                R"("mouse_down","mouse_up","mouse_move","scroll","text"]},)"
                R"("key":{"type":"string"},)"
                R"("button":{"type":"string"},"text":{"type":"string"},)"
                R"("x":{"type":"number"},"y":{"type":"number"},"dx":{"type":"number"},)"
                R"("dy":{"type":"number"}}}}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                if (!args.is_object() || !args.contains("events") || !args["events"].is_array())
                {
                    return std::unexpected(string("expected { events: [ ... ] }"));
                }
                const Json& events = args["events"];
                if (events.empty())
                {
                    return std::unexpected(string("'events' must name at least one event"));
                }
                if (events.size() > MaxInputBatchSize)
                {
                    return std::unexpected(fmt::format(
                        "'events' exceeds the batch limit of {} events", MaxInputBatchSize));
                }
                if (!host.InjectInput)
                {
                    return std::unexpected(string("input injection is not available on this host"));
                }

                // Validate and resolve the whole batch up front, so a structural error rejects the
                // whole call before any event is applied (the mutation verbs' discipline).
                vector<ResolvedEvent> resolved;
                resolved.reserve(events.size());
                for (usize i = 0; i < events.size(); ++i)
                {
                    Result<ResolvedEvent> one = ResolveOne(events[i], i);
                    if (!one)
                    {
                        return std::unexpected(one.error());
                    }
                    resolved.push_back(std::move(*one));
                }

                for (const ResolvedEvent& event : resolved)
                {
                    ApplyResolved(host, event);
                }
                return Json{{"applied", resolved.size()}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }
    }
}
