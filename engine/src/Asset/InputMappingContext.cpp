#include <Veng/Asset/InputMappingContext.h>

namespace Veng
{
    InputMappingContext::InputMappingContext(vector<InputAction> actions, vector<Binding> bindings)
        : m_Actions(std::move(actions)), m_Bindings(std::move(bindings))
    {
        // The resolver reads the actions in declaration order (Plan 00's resolution invariant)
        // and the bindings as-is; ResolvedContext is that pair, built once here.
        m_Resolved.Actions = m_Actions;
        m_Resolved.Bindings = m_Bindings;
    }

    Ref<InputMappingContext> InputMappingContext::Create(vector<InputAction> actions,
                                                         vector<Binding> bindings)
    {
        return Ref<InputMappingContext>(
            new InputMappingContext(std::move(actions), std::move(bindings)));
    }
}
