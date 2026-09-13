// The recorder's backend on every platform that has no hardware video encoder behind a shareable
// surface. The class above is unchanged — it reports itself unavailable and refuses to start with a
// reason, so no preprocessor conditional reaches above engine/src/Capture/.

#include "VideoRecorderBackend.h"

namespace Veng::Capture
{
    Unique<VideoRecorderBackend> CreateVideoRecorderBackend()
    {
        return nullptr;
    }

    bool IsVideoRecorderBackendCompiled()
    {
        return false;
    }
}
