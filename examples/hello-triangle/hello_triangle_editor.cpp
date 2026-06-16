#include <Veng/Module/Module.h>

// The hello-triangle editor module: the game's editor extensions, loaded by the
// editor host beside the game module. It registers the game's editor-only views
// (custom panels, asset editors, inspector widgets) into the EditorRegistry the
// host passes in. This module registers none yet — it proves the editor-module
// load path end to end (dlopen → ABI check → VengModuleRegister with a non-null
// EditorRegistry).
extern "C" void VengModuleRegister(Veng::VengModuleHost* host)
{
    (void)host;
}

VE_EXPORT_MODULE_ABI()
