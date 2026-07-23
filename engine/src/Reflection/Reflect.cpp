#include <Veng/Reflection/Reflect.h>

// FieldCollector's append. It lives here rather than in the class definition
// because the calls to it from Field_ and ArrayField_ do not depend on those member
// templates' parameters: a body spelling `Fields.push_back` in the class would be
// odr-used while the class definition is parsed, instantiating
// `vector<FieldDescriptor>::push_back` in every translation unit that includes
// Reflect.h rather than only in the ones that register a type.

namespace Veng::Detail
{
    void FieldCollector::Add(FieldDescriptor desc)
    {
        Fields.push_back(std::move(desc));
    }
}
