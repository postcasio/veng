#include <Veng/Input/Actions.h>

#include <algorithm>
#include <cmath>

namespace Veng
{
    namespace
    {
        /// @brief Finds a sample by action id, or nullptr when absent.
        const ActionSample* FindSample(const vector<ActionSample>& samples, ActionId id)
        {
            const auto it = std::ranges::find_if(samples, [id](const ActionSample& sample)
                                                 { return sample.Id == id; });
            return it == samples.end() ? nullptr : &*it;
        }

        /// @brief The raw value a source contributes this tick.
        ///
        /// A digital source (keyboard key or button) contributes 1 while down; an analog axis
        /// contributes its axis value. A gamepad source reads through the axis/button surface,
        /// which the current adapter reports as neutral until the device layer lands.
        f32 ReadSource(const InputSource& source, const RawInputView& raw)
        {
            switch (source.Device)
            {
            case InputDeviceType::Keyboard:
                return raw.IsKeyDown(source.Control) ? 1.0f : 0.0f;
            case InputDeviceType::MouseButton:
            case InputDeviceType::GamepadButton:
                return raw.IsButtonDown(source.Device, source.Control) ? 1.0f : 0.0f;
            case InputDeviceType::MouseAxis:
            case InputDeviceType::GamepadAxis:
                return raw.GetAxis(source.Device, source.Control);
            }
            return 0.0f;
        }
    }

    vec2 ActionState::GetValue(ActionId id) const
    {
        const ActionSample* sample = FindSample(Actions, id);
        return sample != nullptr ? sample->Value : vec2{0.0f};
    }

    f32 ActionState::GetAxis(ActionId id) const
    {
        return GetValue(id).x;
    }

    bool ActionState::IsHeld(ActionId id) const
    {
        const ActionSample* sample = FindSample(Actions, id);
        return sample != nullptr &&
               (sample->Phase == ActionPhase::Started || sample->Phase == ActionPhase::Ongoing);
    }

    bool ActionState::WasTriggered(ActionId id) const
    {
        const ActionSample* sample = FindSample(Actions, id);
        return sample != nullptr && sample->Phase == ActionPhase::Started;
    }

    bool ActionState::WasReleased(ActionId id) const
    {
        const ActionSample* sample = FindSample(Actions, id);
        return sample != nullptr && sample->Phase == ActionPhase::Completed;
    }

    bool ActionState::WasTriggeredThisFrame(ActionId id) const
    {
        const ActionSample* sample = FindSample(Actions, id);
        return sample != nullptr && sample->StartedThisFrame;
    }

    bool ActionState::WasReleasedThisFrame(ActionId id) const
    {
        const ActionSample* sample = FindSample(Actions, id);
        return sample != nullptr && sample->ReleasedThisFrame;
    }

    ActionState ResolveActions(std::span<const ResolvedContext> active, const RawInputView& raw,
                               const ActionState& previous)
    {
        ActionState result;

        // Build the deterministic action set: each context's actions in declaration order,
        // an action declared in more than one context keeping its first position. An unbound
        // declared action still gets a sample.
        for (const ResolvedContext& context : active)
        {
            for (const InputAction& action : context.Actions)
            {
                const bool present =
                    std::ranges::any_of(result.Actions, [&action](const ActionSample& sample)
                                        { return sample.Id == action.Id; });
                if (!present)
                {
                    result.Actions.emplace_back(ActionSample{.Id = action.Id});
                }
            }
        }

        // Accumulate values. A higher-priority context (later in active) that binds an action
        // shadows every lower context's bindings of that same action, so find the highest
        // context binding each action and combine only its bindings.
        for (ActionSample& sample : result.Actions)
        {
            const vector<Binding>* winningBindings = nullptr;
            for (const ResolvedContext& context : active)
            {
                const bool bindsAction =
                    std::ranges::any_of(context.Bindings, [&sample](const Binding& binding)
                                        { return binding.Action == sample.Id; });
                if (bindsAction)
                {
                    winningBindings = &context.Bindings;
                }
            }

            if (winningBindings == nullptr)
            {
                continue;
            }

            for (const Binding& binding : *winningBindings)
            {
                if (binding.Action != sample.Id)
                {
                    continue;
                }

                const f32 raw01 = ReadSource(binding.Source, raw);
                const f32 contribution = binding.Scale * raw01;
                switch (binding.Axis)
                {
                case AxisComponent::Whole:
                    // The strongest push wins, rather than the last binding listed. Several
                    // sources may drive one whole-axis action — two keys spelling the same verb,
                    // or a stick beside a key — and the other two components' `+=` is wrong here:
                    // summing would read 2.0 for two keys held at once, and a plain assignment
                    // let a *resting* alternate overwrite a pushed source with its own zero, so
                    // whichever source happened to be listed last was the only live one.
                    if (std::abs(contribution) > std::abs(sample.Value.x))
                    {
                        sample.Value = vec2{contribution, 0.0f};
                    }
                    break;
                case AxisComponent::X:
                    sample.Value.x += contribution;
                    break;
                case AxisComponent::Y:
                    sample.Value.y += contribution;
                    break;
                }
            }
        }

        // Derive each action's phase by comparing this tick's activation against the same
        // action in previous. An action is active when |value| > 0; an action absent from
        // previous is treated as inactive last tick.
        for (ActionSample& sample : result.Actions)
        {
            const bool activeNow =
                std::abs(sample.Value.x) > 0.0f || std::abs(sample.Value.y) > 0.0f;
            const ActionSample* prior = FindSample(previous.Actions, sample.Id);
            const bool activeThen = prior != nullptr && (prior->Phase == ActionPhase::Started ||
                                                         prior->Phase == ActionPhase::Ongoing);

            if (activeNow && !activeThen)
            {
                sample.Phase = ActionPhase::Started;
            }
            else if (activeNow && activeThen)
            {
                sample.Phase = ActionPhase::Ongoing;
            }
            else if (!activeNow && activeThen)
            {
                sample.Phase = ActionPhase::Completed;
            }
            else
            {
                sample.Phase = ActionPhase::None;
            }

            // Seed the frame-accumulated edges from this tick's phase. InputMappingSystem ORs these
            // across a frame's ticks (resetting on the first) so a once-per-frame reader sees an edge
            // a later tick would erase from Phase; a single-tick frame reads identically to Phase.
            sample.StartedThisFrame = sample.Phase == ActionPhase::Started;
            sample.ReleasedThisFrame = sample.Phase == ActionPhase::Completed;
        }

        return result;
    }
}
