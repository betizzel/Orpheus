#pragma once
#include "miniaudio.h"

namespace FFDecoder
{
/** @brief miniaudio custom decoding backend for FFmpeg-supported formats; nullptr when built
 * without ORPHEUS_FFMPEG. */
ma_decoding_backend_vtable *vtable();

/** @brief true when this build can decode via FFmpeg. */
bool available();

/** @brief Silence libavutil's stderr logging (must be called before any decode). */
void quietLogging();
} // namespace FFDecoder
