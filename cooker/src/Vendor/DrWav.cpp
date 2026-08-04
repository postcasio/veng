// The single-header dr_wav implementation, compiled once for the cooker. It decodes a WAV source
// to PCM for a sample-mode AudioClip; it is cooker-only and never linked into libveng, which
// samples the cooked PCM directly. The decode is driven from an in-memory buffer.
#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>
