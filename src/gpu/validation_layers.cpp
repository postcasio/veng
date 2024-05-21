#include "validation_layers.h"
#include <vector>

const std::vector<const char *> *validationLayers = new std::vector({"VK_LAYER_KHRONOS_validation"});

const std::vector<const char *> *getValidationLayers()
{
    return validationLayers;
}