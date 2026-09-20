#pragma once

#include <filesystem>
#include <cstddef>
#include <string>
#include <vector>

namespace Playlist
{

struct Entry
{
  std::string path;              // absolute, resolved
  std::string title;             // display title from #EXTINF, may be empty
  int duration_seconds = 0;      // -1/0 when unknown
};

/** @brief ${XDG_DATA_HOME:-~/.local/share}/orpheus/playlists (created on demand). */
std::filesystem::path playlistDir();

/** @brief Absolute file path for a saved playlist name (sanitised, .m3u8 suffix). */
std::filesystem::path pathForName(const std::string &name);

/** @brief Saved playlist names (no extension), naturally sorted. */
std::vector<std::string> listSaved();

/** @brief Parse an .m3u/.m3u8/.pls file. Relative entries resolve against the file's directory. */
bool load(const std::filesystem::path &file, std::vector<Entry> &out);

/** @brief Write an EXTM3U playlist (UTF-8, absolute paths, #EXTINF lines). Atomic (tmp + rename). */
bool save(const std::filesystem::path &file, const std::vector<Entry> &entries);

bool loadNamed(const std::string &name, std::vector<Entry> &out);
bool saveNamed(const std::string &name, const std::vector<Entry> &entries);
bool removeNamed(const std::string &name);
bool renameNamed(const std::string &from, const std::string &to);

/** @brief Drop entries whose file no longer exists; returns how many were removed. */
size_t pruneMissing(std::vector<Entry> &entries);

} // namespace Playlist
