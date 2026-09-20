// library.hpp
#pragma once

#include "art.hpp"
#include "player.hpp"
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Library
{

/**
 * @brief Read tags for one audio file into a SongMetadata.
 * @param load_art when true, decode embedded or sibling cover art.
 */
SongMetadata loadSongMetadata(const std::string &path, bool load_art = true);

/** @brief Decode embedded cover art first, then a sibling image file. */
bool loadCoverArt(const std::string &song_path, Art::ImageData &out, std::string *resolved_path = nullptr);

struct Track
{
  std::string path;
  std::string title;
  std::string artist;
  int track_number = 0;
  int duration_seconds = 0;
};

struct Album
{
  std::string title;
  std::string artist;
  std::string directory;
  int year = 0;
  int total_seconds = 0;
  /// Number of tracks. Always == tracks.size() locally; for a remote album
  /// summary it is known before `tracks` has been fetched.
  int track_count = 0;
  std::vector<Track> tracks;
};

/** Background recursive scanner over a music root. */
class Scanner
{
public:
  Scanner();
  ~Scanner();
  Scanner(const Scanner &) = delete;
  Scanner &operator=(const Scanner &) = delete;

  /** @brief Kick off (or restart) a background scan. Returns immediately. */
  void start(const std::filesystem::path &root, bool force_rescan = false);
  void cancel();
  bool isScanning() const;
  int filesScanned() const;
  int filesTotal() const;
  std::string statusLine() const;
  /** @brief Snapshot of albums found so far, sorted for display. */
  std::vector<Album> albums() const;
  size_t albumCount() const;
  /** @brief Number of tag rows reused from the on-disk cache in the current scan. */
  int cacheHits() const;
  /** @brief Convert an album to a queue, decoding art only for its first track. */
  static std::vector<SongMetadata> albumToQueue(const Album &album);

private:
  void scan(const std::filesystem::path &root, bool force_rescan);

  mutable std::mutex albums_mutex_;
  std::vector<Album> albums_;
  std::thread worker_;
  std::atomic<bool> cancel_requested_{false};
  std::atomic<bool> scanning_{false};
  std::atomic<int> files_scanned_{0};
  std::atomic<int> files_total_{0};
  std::atomic<int> phase_{0};
  std::atomic<int> cache_hits_{0};
};

} // namespace Library
