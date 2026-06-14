#pragma once

#include <Veng/Cook/Cooker.h>

// Registers the cooker's built-in importers (planset-5 plan 03: just `raw`;
// 06-09 add the texture/mesh/shader/material importers here).

namespace Veng::Cook
{
    void RegisterBuiltinImporters(Cooker& cooker);
}
