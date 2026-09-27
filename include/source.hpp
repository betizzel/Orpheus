// source.hpp: where the UI gets its music from.
//
// The browser, the library tab and the queue used to call Util::listDir and
// Library::* directly against the local filesystem. With remote playback there
// are two providers, so those calls go through this interface instead.
//
// Paths handled by the UI are OPAQUE strings whose meaning belongs to the
// active Source: absolute filesystem paths for LocalSource, root-relative wire
// paths for RemoteSource. The UI must never build one with std::filesystem;
// it uses join() / parent() / isRoot() so the same code drives both.
#pragma once

#include "art.hpp"
#include "library.hpp"
#include "player.hpp"
#include "remote.hpp"
#include "util.hpp"

#include <memory>
#include <string>
#include <vector>

namespace Source
{

/// Progress of whichever library index backs the current source.
struct ScanState
{
  bool scanning = false;
  int done = 0;
  int total = 0;
  int albums = 0;
};

class Provider
{
public:
  virtual ~Provider() = default;

  /// Human label for the header, e.g. "~/Music" or "nas:~/Music".
  virtual const std::string &describe() const = 0;

  /// True when bytes travel over the network; the UI warns before bulk work.
  virtual bool isRemote() const = 0;

  /// True while the provider can still answer (an SSH session can die).
  virtual bool alive() const = 0;

  // --- path algebra ------------------------------------------------------
  virtual std::string root() const = 0;
  virtual std::string join(const std::string &dir, const std::string &name) const = 0;
  virtual std::string parent(const std::string &path) const = 0;
  virtual bool isRoot(const std::string &path) const = 0;

  /// Last component, for display.
  virtual std::string baseName(const std::string &path) const = 0;

  // --- content -----------------------------------------------------------
  virtual bool list(const std::string &path, std::vector<Util::DirEntry> &out) = 0;

  /// True when `path` names a directory (cheap for local, cached for remote).
  virtual bool isDirectory(const std::string &path) = 0;

  /// Tags only; never decodes cover art (that is the slow part).
  virtual SongMetadata metadata(const std::string &path) = 0;
  virtual bool coverArt(const std::string &path, Art::ImageData &out) = 0;

  /**
   * @brief Turn a source path into something miniaudio can open.
   * Local returns the path unchanged; remote returns an "orpheus://" URL that
   * the installed Remote::Vfs resolves.
   */
  virtual std::string playableUri(const std::string &path) const = 0;

  // --- library index -----------------------------------------------------
  virtual bool albums(std::vector<Library::Album> &out) = 0;

  /**
   * @brief Ensure `album.tracks` is populated for the album at `index`.
   * Local albums always are. A remote album arrives as a summary and costs
   * one round trip to expand, so the UI pays it when you open or queue that
   * album rather than N trips just to draw the list.
   */
  virtual bool ensureTracks(size_t index, Library::Album &album) = 0;

  virtual ScanState scanState() = 0;
  virtual void startScan(bool force) = 0;

  /// Expand an album into queue entries (art is loaded lazily per track).
  virtual std::vector<SongMetadata> albumToQueue(const Library::Album &album) = 0;

  /**
   * @brief Recursive name search below `root_path`.
   * @param out    matches as (path, is_directory) pairs
   * @param limit  hard cap on results; the remote walk also stops after a
   *               bounded number of requests, since each level is a round trip.
   */
  virtual bool search(const std::string &root_path,
                      const std::string &query,
                      std::vector<std::pair<std::string, bool>> &out,
                      size_t limit) = 0;
};

/// Filesystem-backed provider; owns a background Library::Scanner.
std::unique_ptr<Provider> makeLocal(const std::string &music_root);

/// orpheusd-backed provider. The session must outlive the provider.
std::unique_ptr<Provider> makeRemote(Remote::Session &session);

} // namespace Source
