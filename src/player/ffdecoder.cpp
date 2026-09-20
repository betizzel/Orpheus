#include "ffdecoder.hpp"

#ifdef ORPHEUS_FFMPEG

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
}

namespace
{
struct IOBridge
{
  ma_read_proc read = nullptr;
  ma_seek_proc seek = nullptr;
  ma_tell_proc tell = nullptr;
  void *user = nullptr;
  const uint8_t *memory = nullptr;
  size_t memory_size = 0;
  size_t memory_cursor = 0;
};

struct FFDataSource
{
  ma_data_source_base base{};
  AVFormatContext *format = nullptr;
  AVCodecContext *codec = nullptr;
  AVStream *stream = nullptr;
  SwrContext *swr = nullptr;
  AVPacket *packet = nullptr;
  AVFrame *frame = nullptr;
  AVIOContext *io = nullptr;
  IOBridge *bridge = nullptr;
  int stream_index = -1;
  int source_rate = 0;
  int source_channels = 0;
  int output_rate = 0;
  int output_channels = 0;
  AVChannelLayout source_layout{};
  AVChannelLayout output_layout{};
  std::vector<float> pcm;
  size_t pcm_cursor_frames = 0;
  ma_uint64 cursor_frames = 0;
  ma_uint64 length_frames = 0;
  bool has_length = false;
  bool input_eof = false;
  bool decoder_input_pending = false;
  bool drain_sent = false;
  bool decoder_eof = false;
  bool swr_eof = false;
  bool custom_io = false;
  bool base_initialized = false;
};

extern ma_data_source_vtable g_data_source_vtable;

static int io_read(void *opaque, uint8_t *buffer, int buffer_size)
{
  auto *bridge = static_cast<IOBridge *>(opaque);
  if (bridge == nullptr || buffer == nullptr || buffer_size <= 0)
    return AVERROR(EINVAL);

  if (bridge->memory != nullptr)
  {
    const size_t remaining = bridge->memory_size - std::min(bridge->memory_cursor, bridge->memory_size);
    const size_t count = std::min(remaining, static_cast<size_t>(buffer_size));
    if (count == 0)
      return AVERROR_EOF;
    std::memcpy(buffer, bridge->memory + bridge->memory_cursor, count);
    bridge->memory_cursor += count;
    return static_cast<int>(count);
  }

  if (bridge->read == nullptr)
    return AVERROR(EIO);
  size_t bytes_read = 0;
  const ma_result result = bridge->read(bridge->user, buffer, static_cast<size_t>(buffer_size), &bytes_read);
  if (result != MA_SUCCESS && bytes_read == 0)
    return AVERROR(EIO);
  if (bytes_read == 0)
    return AVERROR_EOF;
  return static_cast<int>(bytes_read);
}

static int64_t io_seek(void *opaque, int64_t offset, int whence)
{
  auto *bridge = static_cast<IOBridge *>(opaque);
  if (bridge == nullptr)
    return AVERROR(EINVAL);

  if ((whence & AVSEEK_SIZE) != 0)
  {
    if (bridge->memory != nullptr)
      return static_cast<int64_t>(bridge->memory_size);
    return AVERROR(ENOSYS);
  }

  if (bridge->memory != nullptr)
  {
    int64_t target = offset;
    if (whence == SEEK_CUR)
      target += static_cast<int64_t>(bridge->memory_cursor);
    else if (whence == SEEK_END)
      target += static_cast<int64_t>(bridge->memory_size);
    if (target < 0 || static_cast<uint64_t>(target) > bridge->memory_size)
      return AVERROR(EINVAL);
    bridge->memory_cursor = static_cast<size_t>(target);
    return target;
  }

  if (bridge->seek == nullptr || bridge->tell == nullptr)
    return AVERROR(ENOSYS);
  ma_seek_origin origin = ma_seek_origin_start;
  if (whence == SEEK_CUR)
    origin = ma_seek_origin_current;
  else if (whence == SEEK_END)
    origin = ma_seek_origin_end;
  if (bridge->seek(bridge->user, static_cast<ma_int64>(offset), origin) != MA_SUCCESS)
    return AVERROR(EIO);
  ma_int64 cursor = 0;
  if (bridge->tell(bridge->user, &cursor) != MA_SUCCESS)
    return AVERROR(EIO);
  return cursor;
}

static void free_io(FFDataSource *source)
{
  if (source->io != nullptr)
    avio_context_free(&source->io);
  delete source->bridge;
  source->bridge = nullptr;
}

static void release_source(FFDataSource *source)
{
  if (source == nullptr)
    return;
  if (source->format != nullptr)
    avformat_close_input(&source->format);
  if (source->codec != nullptr)
    avcodec_free_context(&source->codec);
  if (source->swr != nullptr)
    swr_free(&source->swr);
  if (source->packet != nullptr)
    av_packet_free(&source->packet);
  if (source->frame != nullptr)
    av_frame_free(&source->frame);
  if (source->custom_io)
    free_io(source);
  av_channel_layout_uninit(&source->source_layout);
  av_channel_layout_uninit(&source->output_layout);
  if (source->base_initialized)
    ma_data_source_uninit(&source->base);
  source->base_initialized = false;
  delete source;
}

static ma_result setup_source(FFDataSource *source, const ma_decoding_backend_config *config)
{
  if (source == nullptr || source->format == nullptr)
    return MA_INVALID_ARGS;

  source->stream_index = av_find_best_stream(source->format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (source->stream_index < 0)
    return MA_NO_DATA_AVAILABLE;
  source->stream = source->format->streams[source->stream_index];

  const AVCodecParameters *parameters = source->stream->codecpar;
  const AVCodec *decoder = avcodec_find_decoder(parameters->codec_id);
  if (decoder == nullptr)
    return MA_NO_BACKEND;
  source->codec = avcodec_alloc_context3(decoder);
  if (source->codec == nullptr)
    return MA_OUT_OF_MEMORY;
  if (avcodec_parameters_to_context(source->codec, parameters) < 0)
    return MA_INVALID_DATA;
  if (avcodec_open2(source->codec, decoder, nullptr) < 0)
    return MA_INVALID_DATA;

  source->source_rate = source->codec->sample_rate;
  source->source_channels = source->codec->ch_layout.nb_channels;
  if (source->source_rate <= 0 || source->source_channels <= 0)
    return MA_INVALID_DATA;
  if (av_channel_layout_copy(&source->source_layout, &source->codec->ch_layout) < 0)
    return MA_INVALID_DATA;

  source->output_rate = source->source_rate;
  source->output_channels = source->source_channels;
  if (config != nullptr && config->preferredFormat != ma_format_unknown && config->preferredFormat != ma_format_f32)
    return MA_NOT_IMPLEMENTED;
  av_channel_layout_default(&source->output_layout, source->output_channels);

  if (swr_alloc_set_opts2(&source->swr, &source->output_layout, AV_SAMPLE_FMT_FLT, source->output_rate,
                          &source->source_layout, source->codec->sample_fmt, source->source_rate, 0, nullptr) < 0 ||
      source->swr == nullptr || swr_init(source->swr) < 0)
    return MA_INVALID_DATA;

  source->packet = av_packet_alloc();
  source->frame = av_frame_alloc();
  if (source->packet == nullptr || source->frame == nullptr)
    return MA_OUT_OF_MEMORY;
  source->pcm.reserve(static_cast<size_t>(source->output_channels) * 65536U);

  if (source->stream->duration != AV_NOPTS_VALUE && source->stream->duration >= 0)
  {
    source->length_frames = static_cast<ma_uint64>(av_rescale_q(source->stream->duration, source->stream->time_base,
                                                                  AVRational{1, source->output_rate}));
    source->has_length = source->length_frames > 0;
  }
  if (!source->has_length && source->format->duration != AV_NOPTS_VALUE && source->format->duration >= 0)
  {
    source->length_frames = static_cast<ma_uint64>(av_rescale_q(source->format->duration, AV_TIME_BASE_Q,
                                                                  AVRational{1, source->output_rate}));
    source->has_length = source->length_frames > 0;
  }
  return MA_SUCCESS;
}
static ma_result init_base(FFDataSource *source)
{
  ma_data_source_config data_config = ma_data_source_config_init();
  data_config.vtable = &g_data_source_vtable;
  const ma_result result = ma_data_source_init(&data_config, &source->base);
  if (result == MA_SUCCESS)
    source->base_initialized = true;
  return result;
}

static ma_result init_file(void *, const char *path, const ma_decoding_backend_config *config,
                           const ma_allocation_callbacks *, ma_data_source **backend)
{
  if (path == nullptr || backend == nullptr)
    return MA_INVALID_ARGS;
  *backend = nullptr;
  auto *source = new (std::nothrow) FFDataSource();
  if (source == nullptr)
    return MA_OUT_OF_MEMORY;
  if (avformat_open_input(&source->format, path, nullptr, nullptr) < 0 ||
      avformat_find_stream_info(source->format, nullptr) < 0 || init_base(source) != MA_SUCCESS ||
      setup_source(source, config) != MA_SUCCESS)
  {
    release_source(source);
    return MA_INVALID_FILE;
  }
  *backend = reinterpret_cast<ma_data_source *>(source);
  return MA_SUCCESS;
}

static ma_result init_stream(void *, ma_read_proc read, ma_seek_proc seek, ma_tell_proc tell, void *user,
                             const ma_decoding_backend_config *config, const ma_allocation_callbacks *,
                             ma_data_source **backend)
{
  if (read == nullptr || backend == nullptr)
    return MA_INVALID_ARGS;
  *backend = nullptr;
  auto *source = new (std::nothrow) FFDataSource();
  auto *bridge = new (std::nothrow) IOBridge();
  if (source == nullptr || bridge == nullptr)
  {
    delete source;
    delete bridge;
    return MA_OUT_OF_MEMORY;
  }
  bridge->read = read;
  bridge->seek = seek;
  bridge->tell = tell;
  bridge->user = user;
  source->bridge = bridge;
  unsigned char *buffer = static_cast<unsigned char *>(av_malloc(32768));
  if (buffer == nullptr)
  {
    release_source(source);
    return MA_OUT_OF_MEMORY;
  }
  source->io = avio_alloc_context(buffer, 32768, 0, bridge, io_read, nullptr, io_seek);
  source->custom_io = true;
  if (source->io == nullptr)
  {
    av_free(buffer);
    release_source(source);
    return MA_OUT_OF_MEMORY;
  }
  source->format = avformat_alloc_context();
  if (source->format == nullptr)
  {
    release_source(source);
    return MA_OUT_OF_MEMORY;
  }
  source->format->pb = source->io;
  source->format->flags |= AVFMT_FLAG_CUSTOM_IO;
  source->custom_io = true;
  if (avformat_open_input(&source->format, nullptr, nullptr, nullptr) < 0 ||
      avformat_find_stream_info(source->format, nullptr) < 0 || init_base(source) != MA_SUCCESS ||
      setup_source(source, config) != MA_SUCCESS)
  {
    release_source(source);
    return MA_INVALID_FILE;
  }
  *backend = reinterpret_cast<ma_data_source *>(source);
  return MA_SUCCESS;
}

static ma_result init_memory(void *, const void *data, size_t size, const ma_decoding_backend_config *config,
                             const ma_allocation_callbacks *, ma_data_source **backend)
{
  if (data == nullptr || size == 0 || backend == nullptr)
    return MA_INVALID_ARGS;
  *backend = nullptr;
  auto *source = new (std::nothrow) FFDataSource();
  auto *bridge = new (std::nothrow) IOBridge();
  if (source == nullptr || bridge == nullptr)
  {
    delete source;
    delete bridge;
    return MA_OUT_OF_MEMORY;
  }
  bridge->memory = static_cast<const uint8_t *>(data);
  bridge->memory_size = size;
  source->bridge = bridge;
  unsigned char *buffer = static_cast<unsigned char *>(av_malloc(32768));
  if (buffer == nullptr)
  {
    release_source(source);
    return MA_OUT_OF_MEMORY;
  }
  source->io = avio_alloc_context(buffer, 32768, 0, bridge, io_read, nullptr, io_seek);
  source->custom_io = true;
  if (source->io == nullptr)
  {
    av_free(buffer);
    release_source(source);
    return MA_OUT_OF_MEMORY;
  }
  source->format = avformat_alloc_context();
  if (source->format == nullptr)
  {
    release_source(source);
    return MA_OUT_OF_MEMORY;
  }
  source->format->pb = source->io;
  source->format->flags |= AVFMT_FLAG_CUSTOM_IO;
  source->custom_io = true;
  if (avformat_open_input(&source->format, nullptr, nullptr, nullptr) < 0 ||
      avformat_find_stream_info(source->format, nullptr) < 0 || init_base(source) != MA_SUCCESS ||
      setup_source(source, config) != MA_SUCCESS)
  {
    release_source(source);
    return MA_INVALID_FILE;
  }
  *backend = reinterpret_cast<ma_data_source *>(source);
  return MA_SUCCESS;
}

static bool append_frame(FFDataSource *source, const AVFrame *frame)
{
  if (source->pcm_cursor_frames > 0)
  {
    const size_t consumed = source->pcm_cursor_frames * static_cast<size_t>(source->output_channels);
    const size_t remaining = source->pcm.size() - std::min(consumed, source->pcm.size());
    if (remaining > 0)
      std::memmove(source->pcm.data(), source->pcm.data() + consumed, remaining * sizeof(float));
    source->pcm.resize(remaining);
    source->pcm_cursor_frames = 0;
  }
  int capacity = swr_get_out_samples(source->swr, frame->nb_samples);
  if (capacity <= 0)
    capacity = frame->nb_samples + 32;
  const size_t old_size = source->pcm.size();
  source->pcm.resize(old_size + static_cast<size_t>(capacity) * static_cast<size_t>(source->output_channels));
  uint8_t *output = reinterpret_cast<uint8_t *>(source->pcm.data() + old_size);
  uint8_t *outputs[1] = {output};
  const int converted = swr_convert(source->swr, outputs, capacity,
                                    const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
  if (converted < 0)
  {
    source->pcm.resize(old_size);
    return false;
  }
  source->pcm.resize(old_size + static_cast<size_t>(converted) * static_cast<size_t>(source->output_channels));
  return converted > 0;
}

static bool append_swr_tail(FFDataSource *source)
{
  if (source->swr_eof)
    return false;
  int capacity = swr_get_out_samples(source->swr, 0);
  if (capacity <= 0)
  {
    source->swr_eof = true;
    return false;
  }
  const size_t old_size = source->pcm.size();
  source->pcm.resize(old_size + static_cast<size_t>(capacity) * static_cast<size_t>(source->output_channels));
  uint8_t *output = reinterpret_cast<uint8_t *>(source->pcm.data() + old_size);
  uint8_t *outputs[1] = {output};
  const int converted = swr_convert(source->swr, outputs, capacity, nullptr, 0);
  if (converted <= 0)
  {
    source->pcm.resize(old_size);
    source->swr_eof = true;
    return false;
  }
  source->pcm.resize(old_size + static_cast<size_t>(converted) * static_cast<size_t>(source->output_channels));
  return true;
}

static bool decode_more(FFDataSource *source)
{
  for (;;)
  {
    if (source->decoder_input_pending)
    {
      const int receive = avcodec_receive_frame(source->codec, source->frame);
      if (receive == 0)
      {
        if (append_frame(source, source->frame))
          return true;
        continue;
      }
      if (receive == AVERROR(EAGAIN))
        source->decoder_input_pending = false;
      else if (receive == AVERROR_EOF)
      {
        source->decoder_input_pending = false;
        source->decoder_eof = true;
      }
      else
      {
        source->decoder_input_pending = false;
        return false;
      }
    }

    if (source->decoder_eof)
    {
      if (append_swr_tail(source))
        return true;
      return false;
    }

    if (source->input_eof)
    {
      if (!source->drain_sent)
      {
        const int sent = avcodec_send_packet(source->codec, nullptr);
        if (sent < 0 && sent != AVERROR_EOF)
          return false;
        source->drain_sent = true;
        source->decoder_input_pending = true;
        continue;
      }
      source->decoder_eof = true;
      continue;
    }

    int read_result = 0;
    do
    {
      read_result = av_read_frame(source->format, source->packet);
      if (read_result < 0)
      {
        source->input_eof = true;
        break;
      }
    } while (source->packet->stream_index != source->stream_index);
    if (read_result < 0)
      continue;

    const int sent = avcodec_send_packet(source->codec, source->packet);
    av_packet_unref(source->packet);
    if (sent < 0 && sent != AVERROR(EAGAIN))
      return false;
    source->decoder_input_pending = true;
  }
}

static ma_result data_read(ma_data_source *base, void *frames, ma_uint64 frame_count, ma_uint64 *frames_read)
{
  auto *source = reinterpret_cast<FFDataSource *>(base);
  if (frames_read != nullptr)
    *frames_read = 0;
  if (source == nullptr || frames == nullptr || frames_read == nullptr)
    return MA_INVALID_ARGS;

  auto *output = static_cast<float *>(frames);
  ma_uint64 written = 0;
  while (written < frame_count)
  {
    const size_t available_samples = source->pcm.size() - source->pcm_cursor_frames * static_cast<size_t>(source->output_channels);
    const ma_uint64 available_frames = available_samples / static_cast<size_t>(source->output_channels);
    if (available_frames > 0)
    {
      const ma_uint64 count = std::min(frame_count - written, available_frames);
      std::memcpy(output + written * source->output_channels,
                  source->pcm.data() + source->pcm_cursor_frames * source->output_channels,
                  static_cast<size_t>(count) * source->output_channels * sizeof(float));
      source->pcm_cursor_frames += static_cast<size_t>(count);
      source->cursor_frames += count;
      written += count;
      continue;
    }
    if (!decode_more(source))
      break;
  }
  *frames_read = written;
  return written == 0 && source->decoder_eof && source->swr_eof ? MA_AT_END : MA_SUCCESS;
}

static ma_result data_seek(ma_data_source *base, ma_uint64 frame_index)
{
  auto *source = reinterpret_cast<FFDataSource *>(base);
  if (source == nullptr || source->format == nullptr || source->stream == nullptr)
    return MA_INVALID_ARGS;
  const int64_t relative = av_rescale_q(static_cast<int64_t>(frame_index), AVRational{1, source->output_rate},
                                         source->stream->time_base);
  int64_t timestamp = relative;
  if (source->stream->start_time != AV_NOPTS_VALUE)
    timestamp += source->stream->start_time;
  if (avformat_seek_file(source->format, source->stream_index, INT64_MIN, timestamp, timestamp, AVSEEK_FLAG_ANY) < 0)
    return MA_IO_ERROR;
  avcodec_flush_buffers(source->codec);
  swr_close(source->swr);
  if (swr_init(source->swr) < 0)
    return MA_IO_ERROR;
  source->pcm.clear();
  source->pcm_cursor_frames = 0;
  source->cursor_frames = frame_index;
  source->input_eof = false;
  source->decoder_input_pending = false;
  source->drain_sent = false;
  source->decoder_eof = false;
  source->swr_eof = false;
  av_packet_unref(source->packet);
  return MA_SUCCESS;
}

static ma_result data_format(ma_data_source *base, ma_format *format, ma_uint32 *channels, ma_uint32 *sample_rate,
                             ma_channel *channel_map, size_t channel_map_cap)
{
  auto *source = reinterpret_cast<FFDataSource *>(base);
  if (source == nullptr || format == nullptr || channels == nullptr || sample_rate == nullptr)
    return MA_INVALID_ARGS;
  *format = ma_format_f32;
  *channels = static_cast<ma_uint32>(source->output_channels);
  *sample_rate = static_cast<ma_uint32>(source->output_rate);
  if (channel_map != nullptr)
    ma_channel_map_init_standard(ma_standard_channel_map_default, channel_map, channel_map_cap, *channels);
  return MA_SUCCESS;
}

static ma_result data_cursor(ma_data_source *base, ma_uint64 *cursor)
{
  auto *source = reinterpret_cast<FFDataSource *>(base);
  if (source == nullptr || cursor == nullptr)
    return MA_INVALID_ARGS;
  *cursor = source->cursor_frames;
  return MA_SUCCESS;
}

static ma_result data_length(ma_data_source *base, ma_uint64 *length)
{
  auto *source = reinterpret_cast<FFDataSource *>(base);
  if (source == nullptr || length == nullptr)
    return MA_INVALID_ARGS;
  *length = source->has_length ? source->length_frames : 0;
  return source->has_length ? MA_SUCCESS : MA_NOT_IMPLEMENTED;
}

ma_data_source_vtable g_data_source_vtable =
{
  data_read,
  data_seek,
  data_format,
  data_cursor,
  data_length,
  nullptr,
  0
};

static ma_result backend_init(void *user, ma_read_proc read, ma_seek_proc seek, ma_tell_proc tell, void *read_user,
                              const ma_decoding_backend_config *config, const ma_allocation_callbacks *callbacks,
                              ma_data_source **backend)
{
  return init_stream(user, read, seek, tell, read_user, config, callbacks, backend);
}

static ma_decoding_backend_vtable g_backend_vtable =
{
  backend_init,
  init_file,
  nullptr,
  init_memory,
  [](void *, ma_data_source *backend, const ma_allocation_callbacks *)
  {
    release_source(reinterpret_cast<FFDataSource *>(backend));
  }
};
}

namespace FFDecoder
{
ma_decoding_backend_vtable *vtable()
{
  return &g_backend_vtable;
}

bool available()
{
  return true;
}

void quietLogging()
{
  av_log_set_level(AV_LOG_QUIET);
}
}

#else

namespace FFDecoder
{
ma_decoding_backend_vtable *vtable()
{
  return nullptr;
}

bool available()
{
  return false;
}

void quietLogging()
{
}
}

#endif
