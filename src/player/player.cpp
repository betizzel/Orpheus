#include "player.hpp"
#include "ffdecoder.hpp"
#include "util.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
static void tap_process(ma_node *node, const float **inputs, ma_uint32 *input_frames,
                        float **outputs, ma_uint32 *output_frames)
{
  auto *tap = reinterpret_cast<TapNode *>(node);
  const float *input = inputs[0];
  float *output = outputs[0];
  const ma_uint32 frames = *input_frames;
  const ma_uint32 channels = ma_node_get_input_channels(node, 0);
  if (input != nullptr && output != nullptr && frames > 0)
    std::memcpy(output, input, static_cast<size_t>(frames) * channels * sizeof(float));

  if (tap != nullptr && tap->ring != nullptr && input != nullptr && frames > 0)
  {
    if (channels == 1)
      tap->ring->write(input, frames);
    else if (channels == 2)
    {
      float mono[512];
      ma_uint32 offset = 0;
      while (offset < frames)
      {
        const ma_uint32 count = std::min<ma_uint32>(frames - offset, 512);
        for (ma_uint32 i = 0; i < count; ++i)
          mono[i] = (input[(offset + i) * 2] + input[(offset + i) * 2 + 1]) * 0.5F;
        tap->ring->write(mono, count);
        offset += count;
      }
    }
    tap->frames_written.fetch_add(frames, std::memory_order_relaxed);
  }
  *output_frames = frames;
}

static ma_node_vtable g_tap_vtable =
{
  tap_process,
  nullptr,
  1,
  1,
  0
};
}

extern "C" void miniaudio_on_song_end(void *user_data, ma_sound *)
{
  if (user_data != nullptr)
    static_cast<MiniAudioPlayer *>(user_data)->onSongEnd();
}

MiniAudioPlayer::MiniAudioPlayer() = default;

MiniAudioPlayer::~MiniAudioPlayer()
{
  cleanup();
}

bool MiniAudioPlayer::init(ma_vfs *vfs)
{
  if (audio_state.engine_initialized)
    return true;
  FFDecoder::quietLogging();
  ma_decoding_backend_vtable *custom_vtables[1] = {FFDecoder::vtable()};
  ma_resource_manager_config resource_config = ma_resource_manager_config_init();
  resource_config.ppCustomDecodingBackendVTables = custom_vtables;
  resource_config.customDecodingBackendCount = custom_vtables[0] == nullptr ? 0 : 1;
  // A non-null VFS routes every file open through the caller's provider. The
  // remote VFS passes local paths to a default VFS, so this stays correct
  // even when a queue mixes local and remote tracks.
  resource_config.pVFS = vfs;
  ma_result result = ma_resource_manager_init(&resource_config, &audio_state.resource_manager);
  if (result != MA_SUCCESS)
  {
    Util::errorPrint("init resource manager failed: " + std::string(ma_result_description(result)));
    return false;
  }
  audio_state.resource_manager_initialized = true;

  ma_engine_config engine_config = ma_engine_config_init();
  engine_config.pResourceManager = &audio_state.resource_manager;
  result = ma_engine_init(&engine_config, &audio_state.engine);
  if (result != MA_SUCCESS)
  {
    Util::errorPrint("init miniaudio failed: " + std::string(ma_result_description(result)));
    ma_resource_manager_uninit(&audio_state.resource_manager);
    audio_state.resource_manager_initialized = false;
    return false;
  }
  audio_state.engine_initialized = true;
  if (!initVisualizerAudio())
  {
    Util::errorPrint("initVisualizerAudio failed");
    cleanup();
    return false;
  }
  ma_engine_set_volume(&audio_state.engine, volume);
  return true;
}

void MiniAudioPlayer::onSongEnd()
{
  just_ended.store(true, std::memory_order_release);
}

void MiniAudioPlayer::cleanup()
{
  stopCurrent();
  song_queue.clear();
  shuffle_order.clear();
  current_index = -1;
  if (audio_state.visualizer_initialized)
  {
    ma_node_uninit(&audio_state.tap.base, nullptr);
    audio_state.visualizer_initialized = false;
    audio_state.tap.ring = nullptr;
  }
  audio_state.tap_ring.reset();
  if (audio_state.engine_initialized)
  {
    ma_engine_uninit(&audio_state.engine);
    audio_state.engine_initialized = false;
  }
  if (audio_state.resource_manager_initialized)
  {
    ma_resource_manager_uninit(&audio_state.resource_manager);
    audio_state.resource_manager_initialized = false;
  }
  just_ended.store(false, std::memory_order_release);
}

bool MiniAudioPlayer::loadSong(const std::string &path)
{
  if (!audio_state.engine_initialized || !audio_state.visualizer_initialized)
    return false;
  const ma_result result = ma_sound_init_from_file(&audio_state.engine, path.c_str(),
                                                    MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT, nullptr, nullptr,
                                                    &audio_state.sound);
  if (result != MA_SUCCESS)
  {
    Util::errorPrint("Failed to load song: " + std::string(ma_result_description(result)));
    return false;
  }
  audio_state.sound_is_initialized = true;
  if (ma_node_attach_output_bus(&audio_state.sound, 0, &audio_state.tap.base, 0) != MA_SUCCESS)
  {
    ma_sound_uninit(&audio_state.sound);
    audio_state.sound_is_initialized = false;
    Util::errorPrint("Failed to attach audio to visualizer");
    return false;
  }
  if (audio_state.tap_ring != nullptr)
    audio_state.tap_ring->clear();
  just_ended.store(false, std::memory_order_release);
  ma_sound_set_volume(&audio_state.sound, volume);
  ma_sound_set_end_callback(&audio_state.sound, miniaudio_on_song_end, this);
  return true;
}

void MiniAudioPlayer::stopCurrent(bool preserve_index)
{
  if (audio_state.sound_is_initialized)
  {
    ma_sound_stop(&audio_state.sound);
    ma_sound_uninit(&audio_state.sound);
    audio_state.sound_is_initialized = false;
  }
  just_ended.store(false, std::memory_order_release);
  if (!preserve_index)
    current_index = -1;
}

void MiniAudioPlayer::rebuildShuffle(int preferred_index)
{
  shuffle_order.resize(song_queue.size());
  for (size_t i = 0; i < song_queue.size(); ++i)
    shuffle_order[i] = static_cast<int>(i);
  if (shuffle)
    std::shuffle(shuffle_order.begin(), shuffle_order.end(), random_engine);
  if (preferred_index >= 0 && preferred_index < static_cast<int>(song_queue.size()))
  {
    const auto it = std::find(shuffle_order.begin(), shuffle_order.end(), preferred_index);
    if (it != shuffle_order.end() && it != shuffle_order.begin())
      std::iter_swap(shuffle_order.begin(), it);
  }
}

int MiniAudioPlayer::orderedPosition(int index) const
{
  const auto it = std::find(shuffle_order.begin(), shuffle_order.end(), index);
  return it == shuffle_order.end() ? -1 : static_cast<int>(it - shuffle_order.begin());
}

int MiniAudioPlayer::orderedIndex(int position) const
{
  return position >= 0 && position < static_cast<int>(shuffle_order.size()) ? shuffle_order[position] : -1;
}

bool MiniAudioPlayer::playIndex(int index)
{
  if (index < 0 || index >= static_cast<int>(song_queue.size()) || !audio_state.engine_initialized)
    return false;
  stopCurrent();
  current_index = index;
  if (!loadSong(song_queue[index].song_path))
    return false;
  if (ma_sound_start(&audio_state.sound) != MA_SUCCESS)
  {
    stopCurrent();
    return false;
  }
  return true;
}

void MiniAudioPlayer::queueSong(SongMetadata song)
{
  song_queue.push_back(std::move(song));
  rebuildShuffle(current_index);
}

void MiniAudioPlayer::queueSongNext(SongMetadata song)
{
  const int insertion = current_index >= 0 ? current_index + 1 : static_cast<int>(song_queue.size());
  song_queue.insert(song_queue.begin() + insertion, std::move(song));
  if (current_index >= insertion)
    ++current_index;
  rebuildShuffle(current_index);
}

void MiniAudioPlayer::queueSongs(std::vector<SongMetadata> songs)
{
  for (auto &song : songs)
    song_queue.push_back(std::move(song));
  rebuildShuffle(current_index);
}

void MiniAudioPlayer::replaceQueue(std::vector<SongMetadata> songs, int start_index)
{
  stopCurrent(false);
  song_queue = std::move(songs);
  rebuildShuffle(-1);
  if (!song_queue.empty())
  {
    start_index = std::clamp(start_index, 0, static_cast<int>(song_queue.size()) - 1);
    if (!playIndex(start_index))
      playNextOrdered(false); // requested track won't decode: take the next one
  }
}

bool MiniAudioPlayer::removeAt(int index)
{
  if (index < 0 || index >= static_cast<int>(song_queue.size()))
    return false;
  const bool removing_current = index == current_index;
  if (removing_current)
    stopCurrent(false);
  song_queue.erase(song_queue.begin() + index);
  if (!removing_current && index < current_index)
    --current_index;
  if (song_queue.empty())
    current_index = -1;
  rebuildShuffle(current_index);
  return true;
}

bool MiniAudioPlayer::moveItem(int from, int to)
{
  if (from < 0 || to < 0 || from >= static_cast<int>(song_queue.size()) || to >= static_cast<int>(song_queue.size()))
    return false;
  if (from == to)
    return true;
  auto item = std::move(song_queue[from]);
  song_queue.erase(song_queue.begin() + from);
  song_queue.insert(song_queue.begin() + to, std::move(item));
  if (current_index == from)
    current_index = to;
  else if (from < current_index && to >= current_index)
    --current_index;
  else if (from > current_index && to <= current_index)
    ++current_index;
  rebuildShuffle(current_index);
  return true;
}

void MiniAudioPlayer::clearQueue()
{
  stopCurrent(false);
  song_queue.clear();
  shuffle_order.clear();
}

bool MiniAudioPlayer::playNextOrdered(bool wrap)
{
  if (song_queue.empty())
    return false;

  const int count = static_cast<int>(shuffle_order.size());
  int position = current_index < 0 ? -1 : orderedPosition(current_index);

  // A file that fails to decode (wrong codec, video-only container, deleted
  // underneath us) must not stall the queue: walk forward until something
  // plays, at most once around.
  for (int attempts = 0; attempts < count; ++attempts)
  {
    int next_position = position + 1;
    if (next_position >= count)
    {
      if (!wrap)
        return false;
      next_position = 0;
    }
    if (playIndex(orderedIndex(next_position)))
      return true;
    position = next_position;
  }
  return false;
}

bool MiniAudioPlayer::nextSong()
{
  const bool wrap = repeat_mode == RepeatMode::All;
  return playNextOrdered(wrap);
}

bool MiniAudioPlayer::prevSong()
{
  if (current_index < 0)
    return false;
  if (!audio_state.sound_is_initialized)
    return playIndex(current_index);

  // "Restart the track" only makes sense mid-playback. Sitting on the last
  // frame after the queue drained, '[' should step back a track instead.
  float position = 0.0F;
  const bool at_end = ma_sound_at_end(&audio_state.sound) == MA_TRUE;
  if (!at_end && ma_sound_get_cursor_in_seconds(&audio_state.sound, &position) == MA_SUCCESS && position > 3.0F)
    return rewind();

  const int current_position = orderedPosition(current_index);
  if (current_position <= 0)
    return rewind();
  return playIndex(orderedIndex(current_position - 1));
}

bool MiniAudioPlayer::pauseSong()
{
  if (!audio_state.sound_is_initialized)
    return false;
  if (ma_sound_is_playing(&audio_state.sound) == MA_TRUE)
    return ma_sound_stop(&audio_state.sound) == MA_SUCCESS;
  if (ma_sound_at_end(&audio_state.sound) == MA_TRUE && ma_sound_seek_to_pcm_frame(&audio_state.sound, 0) != MA_SUCCESS)
    return false;
  return ma_sound_start(&audio_state.sound) == MA_SUCCESS;
}

bool MiniAudioPlayer::isPlaying() const
{
  return audio_state.sound_is_initialized && ma_sound_is_playing(&audio_state.sound) == MA_TRUE;
}

bool MiniAudioPlayer::rewind()
{
  return audio_state.sound_is_initialized && ma_sound_seek_to_pcm_frame(&audio_state.sound, 0) == MA_SUCCESS;
}

bool MiniAudioPlayer::seekRelative(int seconds)
{
  if (!audio_state.sound_is_initialized)
    return false;
  float position = 0.0F;
  float length = 0.0F;
  if (ma_sound_get_cursor_in_seconds(&audio_state.sound, &position) != MA_SUCCESS ||
      ma_sound_get_length_in_seconds(&audio_state.sound, &length) != MA_SUCCESS)
    return false;
  const float target = std::clamp(position + static_cast<float>(seconds), 0.0F, std::max(0.0F, length));
  // seek_to_second converts with the DATA SOURCE's rate. Multiplying by the
  // engine rate and calling seek_to_pcm_frame lands in the wrong place for
  // any file whose rate differs from the device (e.g. 44.1k file, 48k device).
  return ma_sound_seek_to_second(&audio_state.sound, target) == MA_SUCCESS;
}

bool MiniAudioPlayer::seekToPercent(int percent)
{
  if (!audio_state.sound_is_initialized)
    return false;
  float length = 0.0F;
  if (ma_sound_get_length_in_seconds(&audio_state.sound, &length) != MA_SUCCESS)
    return false;
  const int clamped = std::clamp(percent, 0, 100);
  return ma_sound_seek_to_second(&audio_state.sound, length * static_cast<float>(clamped) / 100.0F) == MA_SUCCESS;
}

void MiniAudioPlayer::tick()
{
  if (!just_ended.exchange(false, std::memory_order_acq_rel))
    return;
  if (repeat_mode == RepeatMode::One)
  {
    if (playIndex(current_index))
      return;
    // The track we were repeating stopped decoding; don't wedge on it.
  }
  if (!nextSong())
  {
    // End of queue with repeat off. Stop, but keep the sound loaded: tearing
    // it down here left every transport control dead (play, prev and seek all
    // bail on !sound_is_initialized) with the last track still on screen.
    if (audio_state.sound_is_initialized)
      ma_sound_stop(&audio_state.sound);
  }
}

void MiniAudioPlayer::setVolume(float value)
{
  volume = std::clamp(value, 0.0F, 1.0F);
  if (audio_state.sound_is_initialized)
    ma_sound_set_volume(&audio_state.sound, volume);
}

float MiniAudioPlayer::getVolume() const
{
  return volume;
}

void MiniAudioPlayer::adjustVolume(float delta)
{
  setVolume(volume + delta);
}

RepeatMode MiniAudioPlayer::getRepeat() const
{
  return repeat_mode;
}

void MiniAudioPlayer::cycleRepeat()
{
  if (repeat_mode == RepeatMode::Off)
    repeat_mode = RepeatMode::All;
  else if (repeat_mode == RepeatMode::All)
    repeat_mode = RepeatMode::One;
  else
    repeat_mode = RepeatMode::Off;
}

bool MiniAudioPlayer::isShuffle() const
{
  return shuffle;
}

void MiniAudioPlayer::toggleShuffle()
{
  shuffle = !shuffle;
  rebuildShuffle(current_index);
}

int MiniAudioPlayer::getCurrentIndex() const
{
  return current_index;
}

size_t MiniAudioPlayer::getQueueSize() const
{
  return song_queue.size();
}

bool MiniAudioPlayer::isEmpty() const
{
  return song_queue.empty();
}

const SongMetadata *MiniAudioPlayer::getCurrentSong() const
{
  return current_index >= 0 && current_index < static_cast<int>(song_queue.size()) ? &song_queue[current_index] : nullptr;
}

const SongMetadata *MiniAudioPlayer::getQueueSong(size_t index) const
{
  return index < song_queue.size() ? &song_queue[index] : nullptr;
}

const std::vector<SongMetadata> &MiniAudioPlayer::getQueue() const
{
  return song_queue;
}

int MiniAudioPlayer::getCurrentPositionSeconds() const
{
  float seconds = 0.0F;
  if (audio_state.sound_is_initialized && ma_sound_get_cursor_in_seconds(&audio_state.sound, &seconds) == MA_SUCCESS)
    return std::max(0, static_cast<int>(seconds));
  return 0;
}
int MiniAudioPlayer::getSongLengthSeconds() const
{
  float seconds = 0.0F;
  if (audio_state.sound_is_initialized && ma_sound_get_length_in_seconds(&audio_state.sound, &seconds) == MA_SUCCESS)
    return std::max(0, static_cast<int>(seconds));
  const SongMetadata *song = getCurrentSong();
  return song == nullptr ? 0 : song->duration_seconds;
}

int MiniAudioPlayer::getProgressPercent() const
{
  const int length = getSongLengthSeconds();
  if (length <= 0)
    return 0;
  return std::clamp(getCurrentPositionSeconds() * 100 / length, 0, 100);
}

RingBuffer *MiniAudioPlayer::getRingBuffer()
{
  return audio_state.tap_ring.get();
}

ma_uint32 MiniAudioPlayer::getSampleRate() const
{
  return audio_state.engine_initialized ? ma_engine_get_sample_rate(&audio_state.engine) : 0;
}

bool MiniAudioPlayer::initVisualizerAudio()
{
  ma_node_graph *graph = ma_engine_get_node_graph(&audio_state.engine);
  ma_node *endpoint = ma_node_graph_get_endpoint(graph);
  const ma_uint32 channels = ma_engine_get_channels(&audio_state.engine);
  audio_state.tap_ring = std::make_unique<RingBuffer>(16384);
  audio_state.tap.ring = audio_state.tap_ring.get();
  audio_state.tap.frames_written.store(0, std::memory_order_relaxed);
  ma_uint32 input_channels[1] = {channels};
  ma_uint32 output_channels[1] = {channels};
  ma_node_config config = ma_node_config_init();
  config.vtable = &g_tap_vtable;
  config.pInputChannels = input_channels;
  config.pOutputChannels = output_channels;
  if (ma_node_init(graph, &config, nullptr, &audio_state.tap.base) != MA_SUCCESS)
    return false;
  if (ma_node_attach_output_bus(&audio_state.tap, 0, endpoint, 0) != MA_SUCCESS)
  {
    ma_node_uninit(&audio_state.tap.base, nullptr);
    return false;
  }
  audio_state.visualizer_initialized = true;
  return true;
}
