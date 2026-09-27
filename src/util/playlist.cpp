#include "playlist.hpp"

#include "remote.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace Playlist
{
namespace
{

std::string lowerAscii(std::string_view value)
{
  std::string result;
  result.reserve(value.size());

  for (const unsigned char character : value)
  {
    result.push_back(static_cast<char>(std::tolower(character)));
  }

  return result;
}

std::string trim(std::string value)
{
  size_t first = 0;
  while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0)
  {
    ++first;
  }

  size_t last = value.size();
  while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0)
  {
    --last;
  }

  return value.substr(first, last - first);
}

bool startsWithRemoteUrl(std::string_view value)
{
  const std::string lowered = lowerAscii(value);
  return lowered.starts_with("http://") || lowered.starts_with("https://");
}

/// Tracks queued from a remote source are stored as "orpheus://<wire path>".
/// They are not filesystem paths: never resolve, absolutise or stat them.
bool isRemoteTrack(std::string_view value)
{
  return value.starts_with(Remote::Vfs::kScheme);
}

bool parseInteger(std::string_view value, int &result)
{
  if (value.empty())
  {
    return false;
  }

  const char *begin = value.data();
  const char *end = begin + value.size();
  const auto parsed = std::from_chars(begin, end, result);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool resolveEntryPath(const std::filesystem::path &base,
                      std::string_view value,
                      std::string &result)
{
  std::error_code error;
  std::filesystem::path candidate(value);
  if (!candidate.is_absolute())
  {
    candidate = base / candidate;
  }

  const std::filesystem::path absolute = std::filesystem::absolute(candidate, error);
  if (error)
  {
    Util::errorPrint("Could not resolve playlist entry '" + std::string(value) +
                     "': " + error.message());
    return false;
  }

  result = absolute.lexically_normal().string();
  return true;
}

bool addEntry(const std::filesystem::path &base,
              std::string_view rawPath,
              std::string title,
              int duration,
              std::vector<Entry> &out)
{
  const std::string path = trim(std::string(rawPath));
  if (path.empty())
  {
    return true;
  }
  if (startsWithRemoteUrl(path))
  {
    Util::warningPrint("Skipping remote playlist entry: " + path);
    return true;
  }

  Entry entry;
  if (isRemoteTrack(path))
  {
    entry.path = path;
  }
  else if (!resolveEntryPath(base, path, entry.path))
  {
    return false;
  }

  entry.title = std::move(title);
  entry.duration_seconds = duration;
  out.push_back(std::move(entry));
  return true;
}

std::string sanitiseName(const std::string &name)
{
  std::string result;
  result.reserve(name.size());
  bool pendingSpace = false;

  for (const unsigned char character : name)
  {
    if (character == '\0' || character == '/' || character == '\\')
    {
      continue;
    }
    if (std::isspace(character) != 0)
    {
      pendingSpace = !result.empty();
      continue;
    }
    if (pendingSpace)
    {
      result.push_back(' ');
      pendingSpace = false;
    }
    result.push_back(static_cast<char>(character));
  }

  while (!result.empty() && result.front() == '.')
  {
    result.erase(result.begin());
  }
  while (!result.empty() && result.back() == '.')
  {
    result.pop_back();
  }

  return result.empty() ? "playlist" : result;
}

std::filesystem::path absoluteDirectory(const std::filesystem::path &file)
{
  std::error_code error;
  const std::filesystem::path absolute = std::filesystem::absolute(file, error);
  if (error)
  {
    return file.parent_path();
  }
  return absolute.parent_path();
}

bool loadM3u(const std::filesystem::path &file, std::vector<Entry> &out)
{
  std::ifstream input(file, std::ios::binary);
  if (!input)
  {
    Util::errorPrint("Could not open playlist '" + file.string() + "'");
    return false;
  }

  const std::filesystem::path base = absoluteDirectory(file);
  std::string line;
  int pendingDuration = 0;
  std::string pendingTitle;
  bool firstLine = true;

  while (std::getline(input, line))
  {
    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }
    if (firstLine && line.starts_with("\xEF\xBB\xBF"))
    {
      line.erase(0, 3);
    }
    firstLine = false;

    if (line.empty() || trim(line).empty())
    {
      continue;
    }

    const std::string stripped = trim(line);
    if (stripped.starts_with("#EXTINF:"))
    {
      const std::string_view payload(stripped.data() + 8, stripped.size() - 8);
      const size_t comma = payload.find(',');
      const std::string_view durationText = payload.substr(0, comma);

      pendingDuration = 0;
      if (!parseInteger(durationText, pendingDuration))
      {
        Util::warningPrint("Invalid #EXTINF duration in playlist '" + file.string() + "'");
      }

      pendingTitle =
          comma == std::string_view::npos ? std::string{} : std::string(payload.substr(comma + 1));
      continue;
    }
    if (stripped.front() == '#')
    {
      continue;
    }

    if (!addEntry(base, stripped, std::move(pendingTitle), pendingDuration, out))
    {
      return false;
    }
    pendingTitle.clear();
    pendingDuration = 0;
  }

  if (input.bad())
  {
    Util::errorPrint("Error reading playlist '" + file.string() + "'");
    return false;
  }

  return true;
}

struct PlsRecord
{
  std::string path;
  std::string title;
  int duration = 0;
};

bool parseIndexedKey(std::string_view key, std::string_view prefix, int &index)
{
  if (!key.starts_with(prefix))
  {
    return false;
  }

  const std::string_view suffix = key.substr(prefix.size());
  if (suffix.empty() || !parseInteger(suffix, index) || index < 1)
  {
    return false;
  }
  return true;
}

bool loadPls(const std::filesystem::path &file, std::vector<Entry> &out)
{
  std::ifstream input(file, std::ios::binary);
  if (!input)
  {
    Util::errorPrint("Could not open playlist '" + file.string() + "'");
    return false;
  }

  std::map<int, PlsRecord> records;
  std::string line;

  while (std::getline(input, line))
  {
    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }

    const size_t equals = line.find('=');
    if (equals == std::string::npos)
    {
      continue;
    }

    const std::string key = lowerAscii(trim(line.substr(0, equals)));
    const std::string value = trim(line.substr(equals + 1));
    int index = 0;

    if (parseIndexedKey(key, "file", index))
    {
      records[index].path = value;
    }
    else if (parseIndexedKey(key, "title", index))
    {
      records[index].title = value;
    }
    else if (parseIndexedKey(key, "length", index) && !parseInteger(value, records[index].duration))
    {
      Util::warningPrint("Invalid PLS length in playlist '" + file.string() + "'");
    }
  }

  if (input.bad())
  {
    Util::errorPrint("Error reading playlist '" + file.string() + "'");
    return false;
  }

  const std::filesystem::path base = absoluteDirectory(file);
  for (const auto &[index, record] : records)
  {
    (void)index;
    if (record.path.empty())
    {
      continue;
    }
    if (!addEntry(base, record.path, record.title, record.duration, out))
    {
      return false;
    }
  }

  return true;
}

std::atomic<unsigned long long> temporaryCounter = 0;

} // namespace

std::filesystem::path playlistDir()
{
  const char *dataHome = std::getenv("XDG_DATA_HOME");
  const std::filesystem::path base = dataHome != nullptr && *dataHome != '\0'
                                         ? Util::expandHome(dataHome)
                                         : Util::expandHome("~/.local/share");

  const std::filesystem::path directory = base / "orpheus" / "playlists";

  std::error_code error;
  const std::filesystem::path absolute = std::filesystem::absolute(directory, error);
  if (error)
  {
    Util::errorPrint("Could not resolve playlist directory '" + directory.string() +
                     "': " + error.message());
    return directory;
  }

  return absolute.lexically_normal();
}

std::filesystem::path pathForName(const std::string &name)
{
  return playlistDir() / (sanitiseName(name) + ".m3u8");
}

std::vector<std::string> listSaved()
{
  std::vector<std::string> result;
  const std::filesystem::path directory = playlistDir();
  std::error_code error;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error)
  {
    if (error != std::errc::no_such_file_or_directory)
    {
      Util::errorPrint("Could not list playlist directory '" + directory.string() +
                       "': " + error.message());
    }
    return result;
  }

  for (const auto &entry : iterator)
  {
    std::error_code entryError;
    if (!entry.is_regular_file(entryError) || entryError)
    {
      continue;
    }

    const std::string extension = lowerAscii(entry.path().extension().string());
    if (extension != ".m3u" && extension != ".m3u8" && extension != ".pls")
    {
      continue;
    }
    result.push_back(entry.path().stem().string());
  }

  std::sort(result.begin(), result.end(), [](const std::string &left, const std::string &right) {
    return Util::naturalLess(left, right);
  });

  return result;
}

bool load(const std::filesystem::path &file, std::vector<Entry> &out)
{
  std::vector<Entry> parsed;
  const std::string extension = lowerAscii(file.extension().string());
  bool success = false;

  if (extension == ".pls")
  {
    success = loadPls(file, parsed);
  }
  else if (extension == ".m3u" || extension == ".m3u8")
  {
    success = loadM3u(file, parsed);
  }
  else
  {
    Util::errorPrint("Unsupported playlist extension: '" + file.string() + "'");
    return false;
  }

  if (success)
  {
    out = std::move(parsed);
  }

  return success;
}

bool save(const std::filesystem::path &file, const std::vector<Entry> &entries)
{
  const std::filesystem::path parent =
      file.parent_path().empty() ? std::filesystem::path(".") : file.parent_path();

  std::error_code error;
  std::filesystem::create_directories(parent, error);
  if (error)
  {
    Util::errorPrint("Could not create playlist directory '" + parent.string() +
                     "': " + error.message());
    return false;
  }

  const unsigned long long sequence = ++temporaryCounter;
  const std::filesystem::path temporary =
      parent / ("." + file.filename().string() + ".tmp-" +
                std::to_string(static_cast<unsigned long long>(::getpid())) + "-" +
                std::to_string(sequence));
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output)
  {
    Util::errorPrint("Could not open temporary playlist '" + temporary.string() + "'");
    return false;
  }

  output << "#EXTM3U\n";
  for (const Entry &entry : entries)
  {
    output << "#EXTINF:" << entry.duration_seconds << ',' << entry.title << '\n';

    if (isRemoteTrack(entry.path))
    {
      output << entry.path << '\n';
      continue;
    }

    std::error_code pathError;
    const std::filesystem::path absolute = std::filesystem::absolute(entry.path, pathError);
    if (pathError)
    {
      output.close();
      std::filesystem::remove(temporary);
      Util::errorPrint("Could not resolve playlist entry '" + entry.path +
                       "': " + pathError.message());
      return false;
    }

    output << absolute.lexically_normal().string() << '\n';
  }

  output.flush();
  if (!output)
  {
    output.close();
    std::filesystem::remove(temporary);
    Util::errorPrint("Could not write playlist '" + file.string() + "'");
    return false;
  }
  output.close();

  std::filesystem::rename(temporary, file, error);
  if (error)
  {
    std::filesystem::remove(temporary);
    Util::errorPrint("Could not replace playlist '" + file.string() + "': " + error.message());
    return false;
  }
  return true;
}

bool loadNamed(const std::string &name, std::vector<Entry> &out)
{
  return load(pathForName(name), out);
}

bool saveNamed(const std::string &name, const std::vector<Entry> &entries)
{
  return save(pathForName(name), entries);
}

bool removeNamed(const std::string &name)
{
  const std::filesystem::path file = pathForName(name);
  std::error_code error;
  const bool removed = std::filesystem::remove(file, error);
  if (error)
  {
    Util::errorPrint("Could not remove playlist '" + file.string() + "': " + error.message());
    return false;
  }

  return removed;
}

bool renameNamed(const std::string &from, const std::string &to)
{
  const std::filesystem::path source = pathForName(from);
  const std::filesystem::path destination = pathForName(to);

  std::error_code error;
  std::filesystem::create_directories(destination.parent_path(), error);
  if (error)
  {
    Util::errorPrint("Could not create playlist directory '" + destination.parent_path().string() +
                     "': " + error.message());
    return false;
  }

  std::filesystem::rename(source, destination, error);
  if (error)
  {
    Util::errorPrint("Could not rename playlist '" + source.string() + "' to '" +
                     destination.string() + "': " + error.message());
    return false;
  }

  return true;
}

size_t pruneMissing(std::vector<Entry> &entries)
{
  const auto originalSize = entries.size();

  entries.erase(std::remove_if(entries.begin(),
                               entries.end(),
                               [](const Entry &entry) {
                                 // Remote tracks can only be checked through a live session;
                                 // the caller resolves them against the active source.
                                 if (isRemoteTrack(entry.path))
                                 {
                                   return false;
                                 }

                                 std::error_code error;
                                 const bool present = std::filesystem::exists(entry.path, error);
                                 if (error)
                                 {
                                   Util::warningPrint("Could not check playlist entry '" +
                                                      entry.path + "': " + error.message());
                                   return false;
                                 }

                                 return !present;
                               }),
                entries.end());

  return originalSize - entries.size();
}

} // namespace Playlist
