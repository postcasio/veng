#pragma once

#include <Veng/Cook/Cooker.h>

// Registers the cooker's built-in importers.

namespace Veng::Cook
{
    void RegisterBuiltinImporters(Cooker& cooker);
}
