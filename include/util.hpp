// util.hpp
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Util
{

/// A single entry produced by listDir().
struct DirEntry
{
  std::string name; ///< file name (no trailing slash, even for directories)
  bool is_dir = false;
};

/// Options controlling how a directory is enumerated.
struct ListOptions
{
  bool include_hidden = false; ///< show dot-files
  bool audio_only = true;      ///< drop files that aren't playable/playlists
  bool include_dirs = true;    ///< include sub-directories
};

/**
 * @brief expands a leading '~' to $HOME
 */
std::filesystem::path expandHome(std::string_view path);

/**
 * @brief Enumerate a directory: sub-directories first, then files, each group
 *        sorted with a natural (human) ordering so "track2" precedes "track10".
 * @return false when the path is missing, not a directory, or unreadable.
 */
bool listDir(const std::filesystem::path &path,
             std::vector<DirEntry> &out,
             const ListOptions &opts = {});

/**
 * @brief Lowercase extension of a path/filename, without the dot ("" if none).
 */
std::string extensionOf(std::string_view name);

/**
 * @brief True when the file name has an extension Orpheus can decode.
 *        The set widens when built with FFmpeg (ORPHEUS_FFMPEG).
 */
bool isSupportedAudio(std::string_view name);

/**
 * @brief True for .m3u / .m3u8 / .pls playlist files.
 */
bool isPlaylistFile(std::string_view name);

/**
 * @brief Case-insensitive "human" comparison: digit runs compare numerically.
 */
bool naturalLess(std::string_view a, std::string_view b);

/**
 * @brief Case-insensitive substring test (used by the '/' search).
 */
bool containsNoCase(std::string_view haystack, std::string_view needle);

/**
 * @brief format total_seconds as m:ss (or h:mm:ss past an hour)
 */
std::string formatDuration(int total_seconds);

void debugPrint(const std::string &message);
void infoPrint(const std::string &message);
void errorPrint(const std::string &message);
void warningPrint(const std::string &message);

} // namespace Util
