#!/bin/zsh

export VK_ICD_FILENAMES=vulkansdk/macOS/share/vulkan/icd.d/MoltenVK_icd.json
export VK_LAYER_PATH=vulkansdk/macOS/share/vulkan/explicit_layer.d

./out
