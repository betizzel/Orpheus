// source.cpp: the local and remote implementations of Source::Provider.
#include "source.hpp"

#include <chrono>
#include <filesystem>
#include <unordered_map>

namespace Source
{

namespace
{

// ---------------------------------------------------------------------------
// Local
// ---------------------------------------------------------------------------

class LocalProvider final : public Provider
{
public:
  explicit LocalProvider(const std::string &music_root)
      : root_(music_root), description_(music_root)
  {
  }

  const std::string &describe() const override
  {
    return description_;
  }

  bool isRemote() const override
  {
    return false;
  }

  bool alive() const override
  {
    return true;
  }

  std::string root() const override
  {
    return root_;
  }

  std::string join(const std::string &dir, const std::string &name) const override
  {
    return (std::filesystem::path(dir) / name).string();
  }

  std::string parent(const std::string &path) const override
  {
    std::filesystem::path p(path);

    // "/music/albums/" and "/music/albums" must both yield "/music".
    p = p.has_filename() ? p.parent_path() : p.parent_path().parent_path();
    const std::string text = p.string();

    return text.empty() ? "/" : text;
  }

  bool isRoot(const std::string &path) const override
  {
    return path == "/" || path.empty();
  }

  std::string baseName(const std::string &path) const override
  {
    return std::filesystem::path(path).filename().string();
  }

  bool list(const std::string &path, std::vector<Util::DirEntry> &out) override
  {
    Util::ListOptions opts;
    opts.include_hidden = show_hidden_;

    return Util::listDir(path, out, opts);
  }

  bool isDirectory(const std::string &path) override
  {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
  }

  SongMetadata metadata(const std::string &path) override
  {
    return Library::loadSongMetadata(path, false);
  }

  bool coverArt(const std::string &path, Art::ImageData &out) override
  {
    return Library::loadCoverArt(path, out, nullptr);
  }

  std::string playableUri(const std::string &path) const override
  {
    return path;
  }

  bool albums(std::vector<Library::Album> &out) override
  {
    out = scanner_.albums();
    for (auto &album : out)
    {
      album.track_count = static_cast<int>(album.tracks.size());
    }

    return true;
  }

  bool ensureTracks(size_t, Library::Album &) override
  {
    return true;
  } // always loaded

  ScanState scanState() override
  {
    ScanState state;
    state.scanning = scanner_.isScanning();
    state.done = scanner_.filesScanned();
    state.total = scanner_.filesTotal();
    state.albums = static_cast<int>(scanner_.albumCount());

    return state;
  }

  void startScan(bool force) override
  {
    scanner_.start(root_, force);
  }

  std::vector<SongMetadata> albumToQueue(const Library::Album &album) override
  {
    return Library::Scanner::albumToQueue(album);
  }

  bool search(const std::string &root_path,
              const std::string &query,
              std::vector<std::pair<std::string, bool>> &out,
              size_t limit) override
  {
    out.clear();

    std::error_code ec;
    auto opts = std::filesystem::directory_options::skip_permission_denied;
    std::filesystem::recursive_directory_iterator it(root_path, opts, ec);

    if (ec)
    {
      return false;
    }

    const std::filesystem::recursive_directory_iterator end;
    for (; it != end && out.size() < limit; it.increment(ec))
    {
      if (ec)
      {
        break;
      }

      if (it->is_symlink(ec))
      {
        it.disable_recursion_pending(); // never chase symlink loops
        continue;
      }

      const std::string name = it->path().filename().string();
      if (!show_hidden_ && !name.empty() && name.front() == '.')
      {
        if (it->is_directory(ec))
        {
          it.disable_recursion_pending();
        }

        continue;
      }

      const bool is_dir = it->is_directory(ec);
      if (!is_dir && !Util::isSupportedAudio(name) && !Util::isPlaylistFile(name))
      {
        continue;
      }

      if (!Util::containsNoCase(name, query))
      {
        continue;
      }

      out.emplace_back(it->path().string(), is_dir);
    }

    return true;
  }

  void setShowHidden(bool show)
  {
    show_hidden_ = show;
  }

private:
  std::string root_;
  std::string description_;
  bool show_hidden_ = false;
  Library::Scanner scanner_;
};

// ---------------------------------------------------------------------------
// Remote
// ---------------------------------------------------------------------------

class RemoteProvider final : public Provider
{
public:
  explicit RemoteProvider(Remote::Session &session) : session_(session)
  {
    description_ = session.describe() + " (" + session.label() + ")";
  }

  const std::string &describe() const override
  {
    return description_;
  }

  bool isRemote() const override
  {
    return true;
  }

  bool alive() const override
  {
    return session_.alive();
  }

  std::string root() const override
  {
    return {};
  } // "" is the wire root

  std::string join(const std::string &dir, const std::string &name) const override
  {
    return dir.empty() ? name : dir + "/" + name;
  }

  std::string parent(const std::string &path) const override
  {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
  }

  bool isRoot(const std::string &path) const override
  {
    return path.empty();
  }

  std::string baseName(const std::string &path) const override
  {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }

  bool list(const std::string &path, std::vector<Util::DirEntry> &out) override
  {
    if (!session_.list(path, out))
    {
      return false;
    }

    // Remember which children are directories so isDirectory() doesn't need
    // its own round trip for the common "did I select a folder?" question.
    for (const auto &entry : out)
    {
      dir_cache_[join(path, entry.name)] = entry.is_dir;
    }

    return true;
  }

  bool isDirectory(const std::string &path) override
  {
    const auto it = dir_cache_.find(path);
    if (it != dir_cache_.end())
    {
      return it->second;
    }

    // Unknown: a successful LIST means it is a directory.
    std::vector<Util::DirEntry> probe;
    const bool ok = session_.list(path, probe);
    dir_cache_[path] = ok;

    return ok;
  }

  SongMetadata metadata(const std::string &path) override
  {
    SongMetadata meta;
    if (!session_.tags(path, meta))
    {
      meta.song_name = baseName(path);
      meta.artist_name = "Unknown Artist";
    }

    // song_path must be openable by miniaudio, not by the filesystem.
    meta.song_path = Remote::Vfs::url(path);

    return meta;
  }

  bool coverArt(const std::string &path, Art::ImageData &out) override
  {
    std::string wire;

    // The UI hands back whatever it stored in song_path, which is a URL.
    if (!Remote::Vfs::parse(path.c_str(), wire))
    {
      wire = path;
    }

    return session_.art(wire, out);
  }

  std::string playableUri(const std::string &path) const override
  {
    return Remote::Vfs::url(path);
  }

  bool albums(std::vector<Library::Album> &out) override
  {
    std::vector<Remote::AlbumSummary> summaries;
    if (!session_.albums(summaries))
    {
      return false;
    }

    out.clear();
    out.reserve(summaries.size());

    for (const auto &summary : summaries)
    {
      Library::Album album;
      album.title = summary.title;
      album.artist = summary.artist;
      album.directory = summary.directory;
      album.year = summary.year;
      album.total_seconds = summary.total_seconds;
      album.track_count = summary.track_count;
      out.push_back(std::move(album)); // tracks stay empty until expanded
    }

    return true;
  }

  bool ensureTracks(size_t index, Library::Album &album) override
  {
    if (!album.tracks.empty() || album.track_count == 0)
    {
      return true;
    }

    std::vector<Remote::TrackSummary> tracks;
    if (!session_.albumTracks(static_cast<int>(index), tracks))
    {
      return false;
    }

    album.tracks.reserve(tracks.size());
    for (auto &track : tracks)
    {
      Library::Track out;
      out.path = track.path; // wire path; playableUri() wraps it at queue time
      out.title = track.title;
      out.artist = track.artist;
      out.track_number = track.track_number;
      out.duration_seconds = track.duration_seconds;
      album.tracks.push_back(std::move(out));
    }

    return true;
  }

  ScanState scanState() override
  {
    // The UI polls this every frame from the footer and the library tab.
    // Over SSH that would be ~60 round trips a second, so serve a cached
    // answer and only ask the server twice a second.
    const auto now = std::chrono::steady_clock::now();

    if (have_status_ && now - last_status_ < std::chrono::milliseconds(500))
    {
      return cached_status_;
    }

    last_status_ = now;
    have_status_ = true;
    ScanState fresh;

    if (session_.status(fresh.scanning, fresh.done, fresh.total, fresh.albums))
    {
      cached_status_ = fresh;
    }

    return cached_status_;
  }

  void startScan(bool) override
  {
    session_.startScan();
  }

  std::vector<SongMetadata> albumToQueue(const Library::Album &album) override
  {
    std::vector<SongMetadata> songs;
    songs.reserve(album.tracks.size());

    for (const auto &track : album.tracks)
    {
      SongMetadata song;
      song.song_path = Remote::Vfs::url(track.path);
      song.song_name = track.title;
      song.artist_name = track.artist;
      song.album_name = album.title;
      song.track_number = track.track_number;
      song.duration_seconds = track.duration_seconds;
      songs.push_back(std::move(song));
    }

    return songs;
  }

  bool search(const std::string &root_path,
              const std::string &query,
              std::vector<std::pair<std::string, bool>> &out,
              size_t limit) override
  {
    out.clear();

    // Breadth-first over LIST. Every directory level is a round trip, so this
    // is bounded by requests as well as by hits: a deep remote tree would
    // otherwise freeze the UI thread for minutes.
    constexpr size_t kMaxRequests = 256;
    size_t requests = 0;

    std::vector<std::string> frontier{root_path};
    while (!frontier.empty() && out.size() < limit && requests < kMaxRequests)
    {
      std::vector<std::string> next;
      for (const auto &dir : frontier)
      {
        if (out.size() >= limit || requests >= kMaxRequests)
        {
          break;
        }

        std::vector<Util::DirEntry> entries;
        ++requests;

        if (!session_.list(dir, entries))
        {
          continue;
        }

        for (const auto &entry : entries)
        {
          const std::string child = join(dir, entry.name);
          if (entry.is_dir)
          {
            next.push_back(child);
          }

          if (Util::containsNoCase(entry.name, query) && out.size() < limit)
          {
            out.emplace_back(child, entry.is_dir);
          }
        }
      }

      frontier.swap(next);
    }

    return true;
  }

private:
  Remote::Session &session_;
  std::string description_;
  std::unordered_map<std::string, bool> dir_cache_;

  // Cached STATUS, refreshed at most twice a second (see scanState).
  ScanState cached_status_;
  std::chrono::steady_clock::time_point last_status_{};
  bool have_status_ = false;
};

} // namespace

std::unique_ptr<Provider> makeLocal(const std::string &music_root)
{
  return std::make_unique<LocalProvider>(music_root);
}

std::unique_ptr<Provider> makeRemote(Remote::Session &session)
{
  return std::make_unique<RemoteProvider>(session);
}

} // namespace Source
