#pragma once
#include "art.hpp"
#include "miniaudio.h"
#include "ringbuffer.hpp"
#include <atomic>
#include <memory>
#include <random>
#include <string>
#include <vector>

struct SongMetadata
{
  std::string song_name;
  std::string artist_name;
  std::string album_name;
  std::string song_path;
  std::string album_image_path;
  int track_number = 0;
  int duration_seconds = 0;
  Art::ImageData cached_image;
};

enum class RepeatMode { Off, All, One };

// Custom ma_node that copies incoming audio frames into a RingBuffer.
struct TapNode
{
  ma_node_base base;
  RingBuffer *ring = nullptr;
  std::atomic<uint64_t> frames_written{0};
};

struct PlayerData
{
  ma_resource_manager resource_manager{};
  bool resource_manager_initialized = false;
  ma_engine engine{};
  bool engine_initialized = false;
  ma_sound sound{};
  bool sound_is_initialized = false;
  TapNode tap;
  std::unique_ptr<RingBuffer> tap_ring;
  bool visualizer_initialized = false;
};

extern "C" void miniaudio_on_song_end(void *user_data, ma_sound *sound);

class MiniAudioPlayer
{
public:
  MiniAudioPlayer();
  ~MiniAudioPlayer();
  /// @param vfs optional miniaudio VFS; null uses the default (local files).
  bool init(ma_vfs *vfs = nullptr);
  void cleanup();

  void queueSong(SongMetadata song);
  void queueSongNext(SongMetadata song);
  void queueSongs(std::vector<SongMetadata> songs);
  void replaceQueue(std::vector<SongMetadata> songs, int start_index = 0);
  bool removeAt(int index);
  bool moveItem(int from, int to);
  void clearQueue();

  bool playIndex(int index);
  bool nextSong();
  bool prevSong();
  bool pauseSong();
  bool isPlaying() const;
  bool rewind();
  bool seekRelative(int seconds);
  bool seekToPercent(int percent);
  void tick();

  void setVolume(float v);
  float getVolume() const;
  void adjustVolume(float delta);

  RepeatMode getRepeat() const;
  void cycleRepeat();
  bool isShuffle() const;
  void toggleShuffle();

  int getCurrentIndex() const;
  size_t getQueueSize() const;
  bool isEmpty() const;
  const SongMetadata *getCurrentSong() const;
  const SongMetadata *getQueueSong(size_t index) const;
  const std::vector<SongMetadata> &getQueue() const;

  int getCurrentPositionSeconds() const;
  int getSongLengthSeconds() const;
  int getProgressPercent() const;
  RingBuffer *getRingBuffer();
  ma_uint32 getSampleRate() const;


private:
  PlayerData audio_state;
  int current_index = -1;
  std::atomic<bool> just_ended{false};
  std::vector<SongMetadata> song_queue;
  std::vector<int> shuffle_order;
  RepeatMode repeat_mode = RepeatMode::Off;
  bool shuffle = false;
  float volume = 1.0f;
  std::mt19937 random_engine{std::random_device{}()};

  void stopCurrent(bool preserve_index = true);
  bool loadSong(const std::string &path);
  void rebuildShuffle(int preferred_index = -1);
  void onSongEnd();
  friend void miniaudio_on_song_end(void *user_data, ma_sound *sound);
  int orderedPosition(int index) const;
  int orderedIndex(int position) const;
  bool playNextOrdered(bool wrap);
  bool initVisualizerAudio();
};
