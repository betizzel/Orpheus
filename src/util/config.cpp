#include "config.hpp"

#include "util.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <string_view>
#include <unistd.h>

extern "C"
{
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace Config
{
namespace
{

std::string status;

class LuaState
{
public:
  LuaState() : state_(luaL_newstate())
  {
  }

  ~LuaState()
  {
    if (state_ != nullptr)
    {
      lua_close(state_);
    }
  }

  LuaState(const LuaState &) = delete;
  LuaState &operator=(const LuaState &) = delete;

  lua_State *get() const
  {
    return state_;
  }

private:
  lua_State *state_;
};

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

struct Source
{
  lua_State *state = nullptr;
  int table = 0;

  void push(const char *key) const
  {
    if (table != 0)
    {
      lua_getfield(state, table, key);
    }
    else
    {
      lua_getglobal(state, key);
    }
  }
};

bool readString(const Source &source, const char *key, std::string &value, int &warnings)
{
  source.push(key);
  const bool present = !lua_isnil(source.state, -1);

  if (present)
  {
    if (lua_type(source.state, -1) != LUA_TSTRING)
    {
      Util::warningPrint(std::string("Config key '") + key + "' must be a string");
      ++warnings;
    }
    else
    {
      value = lua_tostring(source.state, -1);
    }
  }

  lua_pop(source.state, 1);
  return present;
}

bool readBoolean(const Source &source, const char *key, bool &value, int &warnings)
{
  source.push(key);
  const bool present = !lua_isnil(source.state, -1);

  if (present)
  {
    if (!lua_isboolean(source.state, -1))
    {
      Util::warningPrint(std::string("Config key '") + key + "' must be a boolean");
      ++warnings;
    }
    else
    {
      value = lua_toboolean(source.state, -1) != 0;
    }
  }

  lua_pop(source.state, 1);
  return present;
}

bool readNumber(const Source &source, const char *key, double &value, int &warnings)
{
  source.push(key);
  const bool present = !lua_isnil(source.state, -1);
  bool valid = false;

  if (present)
  {
    int isNumber = 0;
    const lua_Number number = lua_tonumberx(source.state, -1, &isNumber);

    if (!isNumber || !std::isfinite(static_cast<double>(number)))
    {
      Util::warningPrint(std::string("Config key '") + key + "' must be a finite number");
      ++warnings;
    }
    else
    {
      value = static_cast<double>(number);
      valid = true;
    }
  }

  lua_pop(source.state, 1);
  return valid;
}

bool readEnum(const Source &source,
              const char *key,
              std::string &value,
              std::initializer_list<std::string_view> allowed,
              int &warnings)
{
  std::string candidate;
  if (!readString(source, key, candidate, warnings))
  {
    return false;
  }

  const std::string lowered = lowerAscii(candidate);
  for (const std::string_view option : allowed)
  {
    if (lowered == option)
    {
      value = std::string(option);
      return true;
    }
  }

  Util::warningPrint("Unknown value '" + candidate + "' for config key '" + key + "'");
  ++warnings;
  return true;
}

} // namespace

std::filesystem::path configPath()
{
  const char *configHome = std::getenv("XDG_CONFIG_HOME");
  const std::filesystem::path base = configHome != nullptr && *configHome != '\0'
                                         ? Util::expandHome(configHome)
                                         : Util::expandHome("~/.config");

  return base / "orpheus" / "orpheus.lua";
}

Settings load()
{
  Settings settings;

  const std::filesystem::path file = configPath();
  std::error_code fileError;
  const bool exists = std::filesystem::exists(file, fileError);

  if (fileError)
  {
    status =
        "Could not inspect " + file.string() + ": " + fileError.message() + "; using defaults.";
    Util::errorPrint(status);
    return settings;
  }
  if (!exists)
  {
    status = "No config file at " + file.string() + "; using defaults.";
    return settings;
  }

  LuaState lua;
  if (lua.get() == nullptr)
  {
    status = "Could not create a Lua state; using defaults.";
    Util::errorPrint(status);
    return settings;
  }

  luaL_openlibs(lua.get());

  const int loadResult = luaL_dofile(lua.get(), file.string().c_str());
  if (loadResult != LUA_OK)
  {
    const char *message = lua_tostring(lua.get(), -1);
    status = "Lua error in " + file.string() + ": " +
             (message != nullptr ? message : "unknown error") + "; using defaults.";
    Util::errorPrint(status);
    return settings;
  }

  const bool returnedTable = lua_istable(lua.get(), -1);
  const Source source{lua.get(), returnedTable ? lua_absindex(lua.get(), -1) : 0};
  int warnings = 0;

  readString(source, "music_dir", settings.music_dir, warnings);
  if (settings.music_dir.empty())
  {
    Util::warningPrint("Config key 'music_dir' cannot be empty; using default");
    settings.music_dir = "~/Music";
    ++warnings;
  }

  // Remote source: setting remote_host attaches to an orpheusd over ssh at
  // startup, so you don't pass --remote every time. Empty plays locally.
  readString(source, "remote_host", settings.remote_host, warnings);
  readString(source, "remote_command", settings.remote_command, warnings);
  if (settings.remote_command.empty())
  {
    settings.remote_command = "orpheusd --stdio";
  }

  readEnum(source, "art_color_mode", settings.art_color_mode, {"ansi", "grayscale"}, warnings);
  readEnum(source, "art_render_mode", settings.art_render_mode, {"block", "detailed"}, warnings);
  readEnum(source,
           "visualizer",
           settings.visualizer,
           {"block", "ansi", "braille", "spectrogram"},
           warnings);

  readBoolean(source, "dynamic_palette", settings.dynamic_palette, warnings);
  readBoolean(source, "show_hidden", settings.show_hidden, warnings);

  double number = 0.0;
  if (readNumber(source, "fps", number, warnings))
  {
    if (number < 5.0)
    {
      settings.fps = 5;
    }
    else if (number > 120.0)
    {
      settings.fps = 120;
    }
    else
    {
      settings.fps = static_cast<int>(number);
    }
  }

  if (readNumber(source, "volume", number, warnings))
  {
    if (number < 0.0)
    {
      settings.volume = 0.0f;
    }
    else if (number > 1.0)
    {
      settings.volume = 1.0f;
    }
    else
    {
      settings.volume = static_cast<float>(number);
    }
  }

  readBoolean(source, "shuffle", settings.shuffle, warnings);
  readEnum(source, "repeat_mode", settings.repeat, {"off", "all", "one"}, warnings);
  readBoolean(source, "scan_on_start", settings.scan_on_start, warnings);

  status = "Loaded " + file.string() + (returnedTable ? " (returned table)" : " (global settings)");
  if (warnings != 0)
  {
    status += "; " + std::to_string(warnings) + " invalid value(s) used defaults";
  }

  return settings;
}

bool writeDefaultConfig()
{
  const std::filesystem::path file = configPath();
  const std::filesystem::path directory = file.parent_path();

  std::error_code directoryError;
  std::filesystem::create_directories(directory, directoryError);
  if (directoryError)
  {
    Util::errorPrint("Could not create config directory '" + directory.string() +
                     "': " + directoryError.message());
    return false;
  }

  constexpr std::string_view sample =
      R"lua(-- Orpheus configuration. This file is Lua and must return a table.
return {
  -- Directory searched for music files.
  music_dir = "~/Music",
  -- Cover-art colours: "ansi" or "grayscale".
  art_color_mode = "ansi",
  -- Cover-art renderer: "block" or "detailed".
  art_render_mode = "block",
  -- Visualizer: "block", "ansi", "braille", or "spectrogram".
  visualizer = "block",
  -- Use the current album art palette for equalizer colours.
  dynamic_palette = true,
  -- Include dot-files in the file browser.
  show_hidden = false,
  -- Maximum UI redraw rate, from 5 to 120 frames per second.
  fps = 30,
  -- Playback volume, from 0.0 to 1.0.
  volume = 1.0,
  shuffle = false,
  -- Repeat mode: "off", "all", or "one".
  repeat_mode = "off",
  -- Start a background library scan when Orpheus launches.
  scan_on_start = true,
}
)lua";

  const int descriptor = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (descriptor < 0)
  {
    if (errno == EEXIST)
    {
      return false;
    }

    Util::errorPrint("Could not create config '" + file.string() +
                     "': " + std::string(std::strerror(errno)));
    return false;
  }

  size_t written = 0;
  bool success = true;

  while (written < sample.size())
  {
    const ssize_t count = ::write(descriptor, sample.data() + written, sample.size() - written);
    if (count < 0 && errno == EINTR)
    {
      continue;
    }
    if (count <= 0)
    {
      success = false;
      break;
    }

    written += static_cast<size_t>(count);
  }

  if (::close(descriptor) != 0)
  {
    success = false;
  }
  if (!success)
  {
    Util::errorPrint("Could not write config '" + file.string() +
                     "': " + std::string(std::strerror(errno)));

    std::error_code removeError;
    std::filesystem::remove(file, removeError);
    return false;
  }

  return true;
}

const std::string &lastStatus()
{
  return status;
}

} // namespace Config
