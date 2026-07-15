#include <Veng/Asset/InputMappingContext.h>

namespace Veng
{
    InputMappingContext::InputMappingContext(vector<InputAction> actions, vector<Binding> bindings,
                                             const bool requiresGameplayFocus)
        : m_Actions(std::move(actions)), m_Bindings(std::move(bindings))
    {
        // The resolver reads the actions in declaration order and the bindings as-is;
        // ResolvedContext is that pair plus the focus gate, built once here.
        m_Resolved.Actions = m_Actions;
        m_Resolved.Bindings = m_Bindings;
        m_Resolved.RequiresGameplayFocus = requiresGameplayFocus;
    }

    Ref<InputMappingContext> InputMappingContext::Create(vector<InputAction> actions,
                                                         vector<Binding> bindings,
                                                         const bool requiresGameplayFocus)
    {
        return Ref<InputMappingContext>(new InputMappingContext(
            std::move(actions), std::move(bindings), requiresGameplayFocus));
    }
}
