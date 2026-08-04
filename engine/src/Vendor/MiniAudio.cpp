// The single-header miniaudio implementation, compiled once for the whole library. The high-level
// engine and the encoders are disabled: veng drives ma_device directly and owns its own mixing and
// spatialization DSP, so nothing above the device layer is used.
#define MA_NO_ENGINE
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
