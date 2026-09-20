#pragma once

#include <filesystem>
#include <string>

namespace Config
{

struct Settings
{
  std::string music_dir = "~/Music";
  std::string art_color_mode = "ansi";      // "ansi" | "grayscale"
  std::string art_render_mode = "block";    // "block" | "detailed"
  std::string visualizer = "block";         // "block"|"ansi"|"braille"|"spectrogram"
  bool dynamic_palette = true;              // EQ colours taken from album art
  bool show_hidden = false;                 // show dot-files in the browser
  int fps = 30;                             // UI redraw cap, clamped 5..120
  float volume = 1.0f;                      // 0.0 .. 1.0
  bool shuffle = false;
  std::string repeat = "off";               // "off" | "all" | "one"
  bool scan_on_start = true;                // kick off library scan at launch
  std::string remote_host;                  // non-empty: attach to orpheusd over ssh
  std::string remote_command = "orpheusd --stdio"; // command run on the remote host
};

/** @brief ${XDG_CONFIG_HOME:-~/.config}/orpheus/orpheus.lua */
std::filesystem::path configPath();

/** @brief Load settings; a missing file yields defaults. Bad values warn and fall back. */
Settings load();

/** @brief Write a documented default orpheus.lua if none exists. Returns true when created. */
bool writeDefaultConfig();

/** @brief Human-readable note about what happened during the last load() (for the help screen). */
const std::string &lastStatus();

} // namespace Config
