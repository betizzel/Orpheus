// util.cpp
#include "util.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string>

namespace Util
{

namespace
{

// Formats miniaudio decodes natively. Always available.
constexpr std::array<std::string_view, 4> kNativeAudio = {"mp3", "flac", "wav", "wave"};

#ifdef ORPHEUS_FFMPEG
// Formats that need the FFmpeg decoding backend (src/player/ffdecoder.cpp).
// Deliberately no bare ".mp4": those are overwhelmingly video containers and
// listing them as tracks just fills the browser with things that won't play.
constexpr std::array<std::string_view, 13> kFFmpegAudio = {"m4a", "m4b",  "aac", "alac", "opus", "ogg", "oga",
                                                           "wma", "aiff", "aif", "mka",  "wv",   "ape"};
#endif

constexpr std::array<std::string_view, 3> kPlaylistExt = {"m3u", "m3u8", "pls"};

inline char lower(char c)
{
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

} // namespace

std::filesystem::path expandHome(std::string_view path)
{
  // early return if no ~
  if (path.empty() || path[0] != '~')
    return path;

  const char *home = getenv("HOME");
  if (!home)
    return path;

  // tilde only
  if (path.size() == 1)
    return std::filesystem::path(home);

  return std::filesystem::path(home) / path.substr(2);
}

std::string extensionOf(std::string_view name)
{
  size_t dot = name.find_last_of('.');
  size_t slash = name.find_last_of('/');
  if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
    return {};

  std::string ext(name.substr(dot + 1));
  std::transform(ext.begin(), ext.end(), ext.begin(), lower);
  return ext;
}

bool isSupportedAudio(std::string_view name)
{
  const std::string ext = extensionOf(name);
  if (ext.empty())
    return false;

  for (auto e : kNativeAudio)
    if (ext == e)
      return true;

#ifdef ORPHEUS_FFMPEG
  for (auto e : kFFmpegAudio)
    if (ext == e)
      return true;
#endif
  return false;
}

bool isPlaylistFile(std::string_view name)
{
  const std::string ext = extensionOf(name);
  for (auto e : kPlaylistExt)
    if (ext == e)
      return true;
  return false;
}

bool naturalLess(std::string_view a, std::string_view b)
{
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size())
  {
    unsigned char ca = static_cast<unsigned char>(a[i]);
    unsigned char cb = static_cast<unsigned char>(b[j]);

    if (std::isdigit(ca) && std::isdigit(cb))
    {
      // compare the full digit runs numerically, ignoring leading zeros
      size_t ia = i, ib = j;
      while (ia < a.size() && std::isdigit(static_cast<unsigned char>(a[ia])))
        ++ia;
      while (ib < b.size() && std::isdigit(static_cast<unsigned char>(b[ib])))
        ++ib;

      std::string_view da = a.substr(i, ia - i);
      std::string_view db = b.substr(j, ib - j);
      size_t za = da.find_first_not_of('0');
      size_t zb = db.find_first_not_of('0');
      da = (za == std::string_view::npos) ? std::string_view("0") : da.substr(za);
      db = (zb == std::string_view::npos) ? std::string_view("0") : db.substr(zb);

      if (da.size() != db.size())
        return da.size() < db.size();
      if (da != db)
        return da < db;

      i = ia;
      j = ib;
      continue;
    }

    char la = lower(static_cast<char>(ca));
    char lb = lower(static_cast<char>(cb));
    if (la != lb)
      return la < lb;
    ++i;
    ++j;
  }
  return (a.size() - i) < (b.size() - j);
}

bool containsNoCase(std::string_view haystack, std::string_view needle)
{
  if (needle.empty())
    return true;
  if (needle.size() > haystack.size())
    return false;

  auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                        [](char x, char y) { return lower(x) == lower(y); });
  return it != haystack.end();
}

bool listDir(const std::filesystem::path &path, std::vector<DirEntry> &out, const ListOptions &opts)
{
  out.clear();

  std::error_code ec;
  if (!std::filesystem::is_directory(path, ec))
  {
    errorPrint("Not a readable directory: " + path.string());
    return false;
  }

  // skip_permission_denied keeps a locked-down subdirectory from aborting the
  // whole listing. directory_iterator uses d_type, so is_directory() below is
  // usually free (no extra stat).
  std::filesystem::directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec)
  {
    errorPrint("Filesystem error: " + ec.message() + " (" + path.string() + ")");
    return false;
  }

  std::vector<DirEntry> dirs;
  std::vector<DirEntry> files;

  for (const auto &entry : it)
  {
    std::string name = entry.path().filename().string();
    if (name.empty())
      continue;
    if (!opts.include_hidden && name.front() == '.')
      continue;

    std::error_code type_ec;
    if (entry.is_directory(type_ec))
    {
      if (opts.include_dirs)
        dirs.push_back({std::move(name), true});
      continue;
    }

    if (!entry.is_regular_file(type_ec) && !entry.is_symlink())
      continue;

    if (opts.audio_only && !isSupportedAudio(name) && !isPlaylistFile(name))
      continue;

    files.push_back({std::move(name), false});
  }

  auto by_name = [](const DirEntry &a, const DirEntry &b) { return naturalLess(a.name, b.name); };
  std::sort(dirs.begin(), dirs.end(), by_name);
  std::sort(files.begin(), files.end(), by_name);

  out.reserve(dirs.size() + files.size());
  out.insert(out.end(), std::make_move_iterator(dirs.begin()), std::make_move_iterator(dirs.end()));
  out.insert(out.end(), std::make_move_iterator(files.begin()), std::make_move_iterator(files.end()));
  return true;
}

std::string formatDuration(int total_seconds)
{
  if (total_seconds < 0)
    total_seconds = 0;

  int hours = total_seconds / 3600;
  int minutes = (total_seconds % 3600) / 60;
  int seconds = total_seconds % 60;

  if (hours > 0)
    return std::format("{}:{:02d}:{:02d}", hours, minutes, seconds);
  return std::format("{}:{:02d}", minutes, seconds);
}

void debugPrint(const std::string &message)
{
  std::cout << "[DEBUG] " << message << std::endl;
}

void infoPrint(const std::string &message)
{
  std::cout << "[INFO] " << message << std::endl;
}

void errorPrint(const std::string &message)
{
  std::cerr << "[ERROR] " << message << std::endl;
}

void warningPrint(const std::string &message)
{
  std::cerr << "[WARNING] " << message << std::endl;
}

} // namespace Util
