// remote.hpp — the client half of remote playback.
#pragma once

#include "art.hpp"
#include "miniaudio.h"
#include "player.hpp"
#include "proto.hpp"
#include "util.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Remote
{

struct AlbumSummary
{
  std::string artist;
  std::string title;
  std::string directory;
  int year = 0;
  int track_count = 0;
  int total_seconds = 0;
};

struct TrackSummary
{
  std::string path;
  std::string title;
  std::string artist;
  int track_number = 0;
  int duration_seconds = 0;
};

/**
 * @brief One live connection to an orpheusd.
 *
 * Every call is blocking and serialised behind a mutex: the protocol allows
 * exactly one outstanding request, and both the UI thread and miniaudio's
 * decode path issue requests.
 */
class Session
{
public:
  ~Session();
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  /**
   * @brief Spawn `ssh [extra_args...] <host> -- <remote_command>` and handshake.
   * @param extra_args options passed straight to ssh, e.g. {"-p","2222"}, {"-i","/path/key"} or {"-J","bastion"}.
   *        Needed for anything that isn't a plain ~/.ssh/config host.
   * @return nullptr on failure, with `error` describing why.
   */
  static std::unique_ptr<Session> connectSsh(const std::string &host, const std::string &remote_command,
                                             const std::vector<std::string> &extra_args, std::string &error);

  /// Connect to a local orpheusd Unix socket and handshake.
  static std::unique_ptr<Session> connectUnix(const std::filesystem::path &socket_path, std::string &error);

  /// Adopt an already-connected transport (used by tests).
  static std::unique_ptr<Session> adopt(std::unique_ptr<Proto::Stream> stream, const std::string &description,
                                        std::string &error);

  bool alive() const;
  const std::string &label() const { return label_; }
  const std::string &describe() const { return description_; }

  bool list(const std::string &path, std::vector<Util::DirEntry> &out);
  bool tags(const std::string &path, SongMetadata &out);
  bool art(const std::string &path, Art::ImageData &out);
  bool albums(std::vector<AlbumSummary> &out);
  bool albumTracks(int index, std::vector<TrackSummary> &out);
  bool startScan();
  bool status(bool &scanning, int &done, int &total, int &album_count);

  // Byte access; the VFS below is the only normal caller.
  bool open(const std::string &path, uint32_t &handle, uint64_t &size);
  bool read(uint32_t handle, uint64_t offset, size_t length, std::vector<unsigned char> &out);
  bool closeHandle(uint32_t handle);

  /// Requests issued since connect; used to prove read-ahead is working.
  uint64_t requestCount() const;

private:
  Session() = default;

  bool handshake(std::string &error);
  /// Send one request line and read the status line back. Caller holds lock_.
  bool exchange(const std::string &request, std::string &response);
  bool readPayload(const std::string &response, std::vector<unsigned char> &out, bool &had_payload);
  /// Framing error mid-response: unread bytes are still in the stream, so
  /// every later reply would be misaligned. Close it and report failure.
  bool abandon();

  mutable std::mutex lock_;
  std::unique_ptr<Proto::Stream> stream_;
  std::string label_;
  std::string description_;
  uint64_t requests_ = 0;
  pid_t child_ = -1; ///< ssh process, reaped in the destructor
};

/// Virtual File System state
struct VfsState;

/**
 * @brief A miniaudio VFS with all the files on the remote server.
 *
 * Installed as ma_resource_manager_config::pVFS. Because both the native
 * decoders and the FFmpeg backend's stream-based onInit path read through
 * ma_read_proc/ma_seek_proc, every supported format streams and seeks over
 * SSH with no second playback path. Paths without the scheme prefix fall
 * through to a default VFS so local files still work.
 */
class Vfs
{
public:
  explicit Vfs(Session &session);
  ~Vfs();
  Vfs(const Vfs &) = delete;
  Vfs &operator=(const Vfs &) = delete;

  ma_vfs *handle();

  static constexpr const char *kScheme = "orpheus://";
  static std::string url(const std::string &wire_path);
  /// returns false when `uri` is not a remote URL.
  static bool parse(const char *uri, std::string &wire_path);

private:
  std::unique_ptr<VfsState> impl_;
};

} // namespace Remote
