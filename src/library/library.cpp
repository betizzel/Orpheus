// library.cpp
#include "library.hpp"

#include "util.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <climits>
#include <fstream>
#include <map>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <pthread.h>
#include <signal.h>

#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstringlist.h>
#include <taglib/tvariant.h>

namespace Library
{
namespace
{

struct TagData
{
  std::string title;
  std::string artist;
  std::string album;
  int track_number = 0;
  int year = 0;
  int duration_seconds = 0;
};

struct CacheRow
{
  std::int64_t mtime = 0;
  std::uintmax_t size = 0;
  TagData tags;
};

std::string stringValue(const TagLib::PropertyMap &properties, const char *key)
{
  const auto it = properties.find(TagLib::String(key));
  if (it == properties.end() || it->second.isEmpty())
    return {};
  return it->second.front().to8Bit(true);
}

int integerPrefix(std::string value)
{
  const size_t slash = value.find('/');
  if (slash != std::string::npos)
    value.resize(slash);
  if (value.empty())
    return 0;

  int result = 0;
  const char *first = value.data();
  const char *last = first + value.size();
  const auto parsed = std::from_chars(first, last, result);
  return parsed.ec == std::errc{} ? result : 0;
}

std::int64_t fileMtime(const std::filesystem::path &path)
{
  std::error_code ec;
  const auto timestamp = std::filesystem::last_write_time(path, ec);
  if (ec)
    return 0;
  return std::chrono::duration_cast<std::chrono::seconds>(timestamp.time_since_epoch()).count();
}

std::string absolutePath(const std::string &path)
{
  std::error_code ec;
  const std::filesystem::path result = std::filesystem::absolute(path, ec);
  if (ec)
    return path;
  return result.lexically_normal().string();
}

TagData readTags(const std::string &path)
{
  TagData result;
  const std::filesystem::path file_path(path);
  result.title = file_path.stem().string();
  result.artist = "Unknown Artist";

  TagLib::FileRef file(path.c_str());
  if (file.isNull())
    return result;

  // FileRef::tag() is null for valid audio files without a tag block.
  if (file.tag() != nullptr)
  {
    const TagLib::PropertyMap properties = file.properties();
    const std::string title = stringValue(properties, "TITLE");
    const std::string artist = stringValue(properties, "ARTIST");
    result.title = title.empty() ? result.title : title;
    result.artist = artist.empty() ? "Unknown Artist" : artist;
    result.album = stringValue(properties, "ALBUM");
    result.track_number = integerPrefix(stringValue(properties, "TRACKNUMBER"));
    result.year = integerPrefix(stringValue(properties, "DATE"));
    if (result.year == 0)
      result.year = integerPrefix(stringValue(properties, "YEAR"));
  }

  if (const TagLib::AudioProperties *audio = file.audioProperties(); audio != nullptr)
    result.duration_seconds = std::max(0, audio->lengthInSeconds());
  return result;
}

bool decodeEmbeddedArt(const std::string &song_path, Art::ImageData &out)
{
  TagLib::FileRef file(song_path.c_str());
  if (file.isNull() || file.tag() == nullptr)
    return false;

  const TagLib::List<TagLib::VariantMap> pictures = file.complexProperties("PICTURE");
  for (const auto &picture : pictures)
  {
    const auto it = picture.find(TagLib::String("data"));
    if (it == picture.end() || it->second.type() != TagLib::Variant::ByteVector)
      continue;

    const TagLib::ByteVector bytes = it->second.value<TagLib::ByteVector>();
    if (bytes.isEmpty())
      continue;
    if (Art::LoadImageMemory(out, reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()))
      return true;
  }
  return false;
}

std::filesystem::path cachePath()
{
  const char *xdg = std::getenv("XDG_CACHE_HOME");
  if (xdg != nullptr && *xdg != '\0')
    return std::filesystem::path(xdg) / "orpheus" / "library.tsv";

  const char *home = std::getenv("HOME");
  if (home != nullptr && *home != '\0')
    return std::filesystem::path(home) / ".cache" / "orpheus" / "library.tsv";
  return {};
}

std::string escapeField(const std::string &value)
{
  std::string escaped;
  escaped.reserve(value.size());
  for (const char c : value)
  {
    switch (c)
    {
    case '\\':
      escaped += "\\\\";
      break;
    case '\t':
      escaped += "\\t";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    default:
      escaped += c;
      break;
    }
  }
  return escaped;
}

bool unescapeField(const std::string &value, std::string &out)
{
  out.clear();
  out.reserve(value.size());
  bool escaped = false;
  for (const char c : value)
  {
    if (escaped)
    {
      switch (c)
      {
      case '\\':
        out += '\\';
        break;
      case 't':
        out += '\t';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      default:
        return false;
      }
      escaped = false;
    }
    else if (c == '\\')
    {
      escaped = true;
    }
    else
    {
      out += c;
    }
  }
  return !escaped;
}

bool splitCacheLine(const std::string &line, std::vector<std::string> &fields)
{
  fields.clear();
  std::string field;
  bool escaped = false;
  for (const char c : line)
  {
    if (!escaped && c == '\t')
    {
      fields.push_back(std::move(field));
      field.clear();
      continue;
    }
    field += c;
    if (c == '\\')
      escaped = !escaped;
    else
      escaped = false;
  }
  if (escaped)
    return false;
  fields.push_back(std::move(field));
  return fields.size() == 9;
}

bool parseUnsigned(const std::string &value, std::uintmax_t &out)
{
  if (value.empty())
    return false;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), out);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

bool parseSigned(const std::string &value, std::int64_t &out)
{
  if (value.empty())
    return false;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), out);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

bool parseInt(const std::string &value, int &out)
{
  std::int64_t parsed = 0;
  if (!parseSigned(value, parsed) || parsed < 0 || parsed > static_cast<std::int64_t>(INT_MAX))
    return false;
  out = static_cast<int>(parsed);
  return true;
}

std::unordered_map<std::string, CacheRow> loadCache()
{
  std::unordered_map<std::string, CacheRow> cache;
  const std::filesystem::path path = cachePath();
  if (path.empty())
    return cache;

  std::ifstream input(path);
  std::string line;
  std::vector<std::string> fields;
  while (std::getline(input, line))
  {
    if (!splitCacheLine(line, fields))
      continue;

    std::string file_path;
    CacheRow row;
    if (!unescapeField(fields[0], file_path) || !parseSigned(fields[1], row.mtime) ||
        !parseUnsigned(fields[2], row.size) || !unescapeField(fields[3], row.tags.title) ||
        !unescapeField(fields[4], row.tags.artist) || !unescapeField(fields[5], row.tags.album) ||
        !parseInt(fields[6], row.tags.track_number) || !parseInt(fields[7], row.tags.year) ||
        !parseInt(fields[8], row.tags.duration_seconds) || file_path.empty())
      continue;
    cache[std::move(file_path)] = std::move(row);
  }
  return cache;
}

void saveCache(const std::unordered_map<std::string, CacheRow> &cache)
{
  const std::filesystem::path path = cachePath();
  if (path.empty())
    return;

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec)
    return;

  const std::filesystem::path temporary = path.string() + ".tmp";
  std::ofstream output(temporary, std::ios::trunc);
  if (!output)
    return;

  for (const auto &[file_path, row] : cache)
  {
    output << escapeField(file_path) << '\t' << row.mtime << '\t' << row.size << '\t'
           << escapeField(row.tags.title) << '\t' << escapeField(row.tags.artist) << '\t'
           << escapeField(row.tags.album) << '\t' << row.tags.track_number << '\t' << row.tags.year << '\t'
           << row.tags.duration_seconds << '\n';
  }
  output.close();
  if (!output)
    return;

  std::filesystem::rename(temporary, path, ec);
  if (ec)
    std::filesystem::remove(temporary, ec);
}

/// Filename part of a '/'-separated path, without building a filesystem::path
/// (the comparator below runs O(n log n) times per album).
std::string_view fileNameOf(const std::string &path)
{
  const size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string_view(path) : std::string_view(path).substr(slash + 1);
}

void sortAlbums(std::vector<Album> &albums)
{
  for (Album &album : albums)
  {
    std::sort(album.tracks.begin(), album.tracks.end(), [](const Track &a, const Track &b) {
      if (a.track_number != b.track_number)
        return a.track_number < b.track_number;
      return Util::naturalLess(fileNameOf(a.path), fileNameOf(b.path));
    });
  }
  std::sort(albums.begin(), albums.end(), [](const Album &a, const Album &b) {
    if (a.artist != b.artist)
      return Util::naturalLess(a.artist, b.artist);
    if (a.year != b.year)
      return a.year < b.year;
    if (a.title != b.title)
      return Util::naturalLess(a.title, b.title);
    return Util::naturalLess(a.directory, b.directory);
  });
}

} // namespace

SongMetadata loadSongMetadata(const std::string &path, bool load_art)
{
  const std::string full_path = absolutePath(path);
  const TagData tags = readTags(full_path);

  SongMetadata result;
  result.song_name = tags.title;
  result.artist_name = tags.artist;
  result.album_name = tags.album;
  result.song_path = full_path;
  result.track_number = tags.track_number;
  result.duration_seconds = tags.duration_seconds;
  if (load_art)
    loadCoverArt(full_path, result.cached_image, &result.album_image_path);
  return result;
}

bool loadCoverArt(const std::string &song_path, Art::ImageData &out, std::string *resolved_path)
{
  out = {};
  if (resolved_path != nullptr)
    resolved_path->clear();

  const std::string full_path = absolutePath(song_path);
  if (decodeEmbeddedArt(full_path, out))
    return true;

  const std::string image_path = Art::ResolveImage(full_path);
  if (image_path == "no image")
    return false;
  if (!Art::LoadImageFile(out, image_path))
    return false;
  if (resolved_path != nullptr)
    *resolved_path = image_path;
  return true;
}

Scanner::Scanner() = default;

Scanner::~Scanner()
{
  cancel();
}

void Scanner::start(const std::filesystem::path &root, bool force_rescan)
{
  cancel();
  cancel_requested_.store(false, std::memory_order_release);
  files_scanned_.store(0, std::memory_order_relaxed);
  files_total_.store(0, std::memory_order_relaxed);
  phase_.store(0, std::memory_order_relaxed);
  cache_hits_.store(0, std::memory_order_relaxed);
  {
    std::lock_guard lock(albums_mutex_);
    albums_.clear();
  }

  std::error_code ec;
  const std::filesystem::path scan_root = std::filesystem::absolute(root, ec).lexically_normal();
  scanning_.store(true, std::memory_order_release);

  // The worker inherits this thread's signal mask. Block everything while it
  // is spawned so process-directed signals (orpheusd's SIGTERM, the TUI's
  // SIGWINCH) land on the thread that is waiting for them, not on a scanner
  // that would swallow the EINTR meant to wake a blocking read.
  sigset_t all_signals;
  sigset_t previous;
  sigfillset(&all_signals);
  pthread_sigmask(SIG_BLOCK, &all_signals, &previous);
  worker_ = std::thread(&Scanner::scan, this, scan_root, force_rescan);
  pthread_sigmask(SIG_SETMASK, &previous, nullptr);
}

void Scanner::cancel()
{
  cancel_requested_.store(true, std::memory_order_release);
  if (worker_.joinable())
    worker_.join();
  scanning_.store(false, std::memory_order_release);
}

bool Scanner::isScanning() const
{
  return scanning_.load(std::memory_order_acquire);
}

int Scanner::filesScanned() const
{
  return files_scanned_.load(std::memory_order_relaxed);
}

int Scanner::filesTotal() const
{
  return files_total_.load(std::memory_order_relaxed);
}

std::string Scanner::statusLine() const
{
  const int scanned = filesScanned();
  const int total = filesTotal();
  if (!isScanning())
    return "Discovery complete";
  if (phase_.load(std::memory_order_relaxed) == 0)
    return "Scanning " + std::to_string(scanned) + "/" + std::to_string(total) + " — Discovery";
  return "Scanning " + std::to_string(scanned) + "/" + std::to_string(total) + " — Tag reading";
}

std::vector<Album> Scanner::albums() const
{
  std::lock_guard lock(albums_mutex_);
  return albums_;
}

size_t Scanner::albumCount() const
{
  std::lock_guard lock(albums_mutex_);
  return albums_.size();
}

int Scanner::cacheHits() const
{
  return cache_hits_.load(std::memory_order_relaxed);
}

std::vector<SongMetadata> Scanner::albumToQueue(const Album &album)
{
  std::vector<SongMetadata> queue;
  queue.reserve(album.tracks.size());
  for (size_t i = 0; i < album.tracks.size(); ++i)
    queue.push_back(loadSongMetadata(album.tracks[i].path, i == 0));
  return queue;
}

void Scanner::scan(const std::filesystem::path &root, bool force_rescan)
{
  try
  {
    std::unordered_map<std::string, CacheRow> cache;
    if (!force_rescan)
      cache = loadCache();

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator end;
    while (!ec && it != end)
    {
      if (cancel_requested_.load(std::memory_order_acquire))
        break;
      const auto &entry = *it;
      std::error_code entry_ec;
      if (entry.is_regular_file(entry_ec) && Util::isSupportedAudio(entry.path().filename().string()))
        files.push_back(entry.path());
      it.increment(ec);
    }

    if (cancel_requested_.load(std::memory_order_acquire))
    {
      scanning_.store(false, std::memory_order_release);
      return;
    }

    std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
      return Util::naturalLess(a.string(), b.string());
    });
    files_total_.store(static_cast<int>(files.size()), std::memory_order_release);
    phase_.store(1, std::memory_order_release);

    // `found` stays in discovery order so `album_index` can point into it.
    // sorting and copying happen only when a snapshot is published, which is throttled.
    // Doing both per file made indexing O(n^2) in library size.
    std::vector<Album> found;
    std::unordered_map<std::string, size_t> album_index;
    const auto publish = [&]() {
      std::vector<Album> snapshot = found;
      sortAlbums(snapshot);
      std::lock_guard lock(albums_mutex_);
      albums_ = std::move(snapshot);
    };
    constexpr auto kPublishInterval = std::chrono::milliseconds(250);
    auto last_publish = std::chrono::steady_clock::now();
    for (const std::filesystem::path &file : files)
    {
      if (cancel_requested_.load(std::memory_order_acquire))
        break;

      const std::string path = file.lexically_normal().string();
      const std::int64_t mtime = fileMtime(file);
      std::error_code size_ec;
      const std::uintmax_t size = std::filesystem::file_size(file, size_ec);
      TagData tags;
      const auto cached = cache.find(path);
      if (!force_rescan && cached != cache.end() && cached->second.mtime == mtime &&
          !size_ec && cached->second.size == size)
      {
        tags = cached->second.tags;
        cache_hits_.fetch_add(1, std::memory_order_relaxed);
      }
      else
      {
        tags = readTags(path);
        if (!size_ec)
          cache[path] = {mtime, size, tags};
      }

      std::string directory = file.parent_path().string();
      std::string album_title = tags.album.empty() ? file.parent_path().filename().string() : tags.album;
      // '\0' cannot appear in a path, so the joined key is unambiguous.
      std::string key = directory;
      key.push_back('\0');
      key += album_title;
      const auto [slot, inserted] = album_index.try_emplace(std::move(key), found.size());
      if (inserted)
      {
        // Named fields: a positional list silently breaks whenever Album
        // gains a member (track_count did exactly that).
        found.push_back({.title = std::move(album_title),
          .artist = tags.artist,
          .directory = std::move(directory),
          .year = tags.year,
          .total_seconds = 0,
          .track_count = 0,
          .tracks = {}});
      }
      Album &album = found[slot->second];
      if (album.year == 0 && tags.year != 0)
        album.year = tags.year;
      if (album.artist != tags.artist)
        album.artist = "Various Artists";
      album.total_seconds += tags.duration_seconds;
      album.tracks.push_back({path, tags.title, tags.artist, tags.track_number, tags.duration_seconds});
      ++files_scanned_;

      const auto now = std::chrono::steady_clock::now();
      if (now - last_publish >= kPublishInterval)
      {
        publish();
        last_publish = now;
      }
    }

    if (!cancel_requested_.load(std::memory_order_acquire))
    {
      sortAlbums(found);
      {
        std::lock_guard lock(albums_mutex_);
        albums_ = std::move(found); // last snapshot: no copy needed
      }
      saveCache(cache);
    }
  }
  catch (...)
  {
    // A vanished file or inaccessible cache must not terminate the UI thread.
  }
  scanning_.store(false, std::memory_order_release);
}

} // namespace Library
