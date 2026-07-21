#include <Veng/Persistence/StorePatterns.h>

#include <Veng/Log.h>

namespace Veng
{
    namespace Detail
    {
        void ReportUnmatchedBlob(bool& reported, const string_view fileStem, const TypeId type)
        {
            if (reported)
            {
                return;
            }
            reported = true;
            Log::Warn("store family '{}': a stored blob encodes type {:016X}, which the family "
                      "captures no component for; skipped",
                      fileStem, type);
        }
    }

    StoreFamily SingletonFamily(const StoreFamilyId id, string fileStem)
    {
        return StoreFamily{.Id = id, .FileStem = std::move(fileStem), .Version = 1};
    }
}
