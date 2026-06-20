#include <Veng/Module/Module.h>

// The hello-triangle editor module, loaded by the editor host beside the game module.
// Registers no views — its purpose is to exercise the editor-module load path end to end
// (dlopen → ABI check → VengModuleRegister with a non-null EditorRegistry).
extern "C" void VengModuleRegister(Veng::VengModuleHost* host)
{
    (void)host;
}

VE_EXPORT_MODULE_ABI()
