#include "ui.hpp"
#include "art.hpp"
#include "colorpair.hpp"
#include "library.hpp"
#include "player.hpp"
#include "playlist.hpp"
#include "util.hpp"
#include "visualizer.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <iomanip>
#include <ncurses.h>
#include <sstream>

namespace
{

// Layout: the list panes all start below the pane title.
constexpr int LIST_TOP = 3;
constexpr int HEADER_ROWS = 3;
constexpr int FOOTER_ROWS = 3;
constexpr int MIN_ROWS = 12;
constexpr int MIN_COLS = 48;

// grayscale density 0-9 mapped to ANSI-256 gray palette indices
constexpr int GRAYSCALE_ANSI_MAP[10] = {236, 238, 240, 242, 244, 246, 248, 250, 253, 255};

/**
 * @brief Truncate a UTF-8 string so it occupies at most `max_width` terminal
 * columns. Byte-slicing would split multi-byte sequences and leave ncurses
 * printing replacement garbage, which is exactly what happens with the
 * non-ASCII filenames a real music library is full of.
 */
std::string clipToWidth(const std::string &text, int max_width)
{
  if (max_width <= 0)
    return {};

  std::mbstate_t st{};
  const char *p = text.c_str();
  const char *end = p + text.size();
  int width = 0;
  size_t kept = 0;

  while (p < end)
  {
    wchar_t wc = 0;
    size_t consumed = std::mbrtowc(&wc, p, static_cast<size_t>(end - p), &st);
    if (consumed == static_cast<size_t>(-2) || consumed == static_cast<size_t>(-1))
      break; // invalid/incomplete: stop at the last good boundary
    if (consumed == 0)
      consumed = 1;

    int w = wcwidth(wc);
    if (w < 0)
      w = 0; // control chars render as nothing useful; don't count them
    if (width + w > max_width)
      break;

    width += w;
    kept += consumed;
    p += consumed;
  }

  return text.substr(0, kept);
}

/// mvwprintw that never wraps past the right edge of the pane.
void printClipped(WINDOW *win, int y, int x, int max_width, const std::string &text)
{
  if (max_width <= 0)
    return;
  const std::string clipped = clipToWidth(text, max_width);
  mvwprintw(win, y, x, "%s", clipped.c_str());
}

/// Render one list row, highlighting when selected.
void printRow(WINDOW *win, int y, int x, int max_width, const std::string &text, bool selected)
{
  if (selected)
    wattron(win, A_REVERSE | A_BOLD);
  printClipped(win, y, x, max_width, text);
  if (selected)
    wattroff(win, A_REVERSE | A_BOLD);
}

/**
 * @brief Render one AsciiCell to an ncurses window.
 */
void drawCell(WINDOW *win, int y, int x, const Art::AsciiCell &cell, Art::ColorMode mode)
{
  int pair_id = 0;

  switch (mode)
  {
  case Art::ColorMode::GRAYSCALE: {
    // fg/bg are density buckets here, not ANSI indices. Clamp: a malformed
    // cell would otherwise index off the end of the ramp.
    int fg = std::clamp(cell.fg, 0, 9);
    int bg = std::clamp(cell.bg, 0, 9);
    pair_id = getSharedColorPair(GRAYSCALE_ANSI_MAP[fg], GRAYSCALE_ANSI_MAP[bg]);
    break;
  }
  case Art::ColorMode::ANSI_256:
    pair_id = getSharedColorPair(cell.fg, cell.bg);
    break;
  }

  cchar_t cc;
  setcchar(&cc, &cell.ch, 0, static_cast<short>(pair_id), nullptr);
  mvwadd_wch(win, y, x, &cc);
}

void drawAsciiArt(WINDOW *win, int start_y, int start_x, const Art::AsciiCanvas &art)
{
  if (art.cells.empty() || art.width == 0 || art.height == 0)
    return;

  for (int y = 0; y < art.height; ++y)
    for (int x = 0; x < art.width; ++x)
      drawCell(win, start_y + y, start_x + x, art.cells[y * art.width + x], art.mode);
}

const char *repeatLabel(RepeatMode mode)
{
  switch (mode)
  {
  case RepeatMode::All:
    return "all";
  case RepeatMode::One:
    return "one";
  case RepeatMode::Off:
    break;
  }
  return "off";
}

Art::ColorMode colorModeFromString(const std::string &s)
{
  return s == "grayscale" ? Art::ColorMode::GRAYSCALE : Art::ColorMode::ANSI_256;
}

Art::RenderMode renderModeFromString(const std::string &s)
{
  return s == "detailed" ? Art::RenderMode::DETAILED : Art::RenderMode::BLOCK;
}

Visualizer::Style vizStyleFromString(const std::string &s)
{
  if (s == "ansi")
    return Visualizer::Style::ANSI_ART;
  if (s == "braille")
    return Visualizer::Style::BRAILLE;
  if (s == "spectrogram")
    return Visualizer::Style::SPECTROGRAM;
  return Visualizer::Style::BLOCK;
}

RepeatMode repeatFromString(const std::string &s)
{
  if (s == "all")
    return RepeatMode::All;
  if (s == "one")
    return RepeatMode::One;
  return RepeatMode::Off;
}

} // namespace

// ---------------------------------------------------------------------------
// ListView
// ---------------------------------------------------------------------------

void ListView::move(int delta, int count, bool wrap)
{
  if (count <= 0)
  {
    selected = 0;
    scroll = 0;
    return;
  }

  long long next = static_cast<long long>(selected) + delta;
  if (wrap)
  {
    // Single-step moves wrap around the ends; multi-row jumps (ctrl-d/u)
    // saturate so a half-page down near the bottom doesn't teleport to the top.
    if (delta == 1 && next >= count)
      next = 0;
    else if (delta == -1 && next < 0)
      next = count - 1;
  }
  selected = static_cast<int>(std::clamp<long long>(next, 0, count - 1));
}

void ListView::jump(int index, int count)
{
  if (count <= 0)
  {
    selected = 0;
    return;
  }
  selected = std::clamp(index, 0, count - 1);
}

void ListView::clamp(int count, int height)
{
  if (count <= 0 || height <= 0)
  {
    selected = 0;
    scroll = 0;
    return;
  }

  selected = std::clamp(selected, 0, count - 1);
  if (selected < scroll)
    scroll = selected;
  if (selected >= scroll + height)
    scroll = selected - height + 1;
  scroll = std::clamp(scroll, 0, std::max(0, count - height));
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

UIManager::UIManager() = default;

UIManager::~UIManager()
{
  cleanup();
}

void UIManager::destroyWindows()
{
  if (state.header)
    delwin(state.header);
  if (state.main_area)
    delwin(state.main_area);
  if (state.footer)
    delwin(state.footer);
  state.header = nullptr;
  state.main_area = nullptr;
  state.footer = nullptr;
}

void UIManager::cleanup()
{
  // Drop the source before ncurses goes away: the local provider joins its
  // scanner thread here, and the remote one tears down its SSH session.
  state.source.reset();
  destroyWindows();
  endwin();
}

void UIManager::createWindows()
{
  getmaxyx(stdscr, state.max_rows, state.max_cols);

  const int body_rows = std::max(1, state.max_rows - HEADER_ROWS - FOOTER_ROWS);
  state.header = newwin(HEADER_ROWS, state.max_cols, 0, 0);
  state.main_area = newwin(body_rows, state.max_cols, HEADER_ROWS, 0);
  state.footer = newwin(FOOTER_ROWS, state.max_cols, state.max_rows - FOOTER_ROWS, 0);

  if (!state.header || !state.main_area || !state.footer)
  {
    Util::errorPrint("Failed to create ncurses windows");
    endwin();
    exit(1);
  }
}

void UIManager::handleResize()
{
  endwin();
  refresh();
  clear();
  destroyWindows();
  createWindows();

  // Art is rendered at a fixed cell size; a resize invalidates it.
  state.cached_song_path.clear();
  state.viz_state.analyzer.bar_count = 0;
  clearok(stdscr, TRUE);
  refresh();
}

void UIManager::init(const Config::Settings &cfg, std::unique_ptr<Source::Provider> source, ma_vfs *vfs)
{
  state.cfg = cfg;
  state.source = std::move(source);
  Util::debugPrint("initializing UI with source: " + state.source->describe());

  createWindows();
  Util::debugPrint("Terminal dimensions: " + std::to_string(state.max_rows) + " x " + std::to_string(state.max_cols));

  state.music_root = state.source->root();
  state.current_directory = state.music_root;
  state.current_tab = Tab::home;

  state.art_color_mode = colorModeFromString(cfg.art_color_mode);
  state.art_render_mode = renderModeFromString(cfg.art_render_mode);
  state.viz_state.style = vizStyleFromString(cfg.visualizer);
  state.viz_state.use_dynamic_palette = cfg.dynamic_palette;

  start_color();
  use_default_colors();

  // A non-null VFS makes ma_sound_init_from_file resolve "orpheus://" URLs
  // through the remote session; local paths still go to the default VFS.
  if (!state.player.init(vfs))
    Util::errorPrint("Audio engine failed to initialise; playback is unavailable");

  state.player.setVolume(cfg.volume);
  if (cfg.shuffle)
    state.player.toggleShuffle();
  for (RepeatMode want = repeatFromString(cfg.repeat); state.player.getRepeat() != want;)
    state.player.cycleRepeat();

  refreshPlaylists();

  if (cfg.scan_on_start)
    state.source->startScan(false);

  Util::debugPrint("UI initialized successfully");
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

int UIManager::listHeight() const
{
  if (!state.main_area)
    return 0;
  return std::max(0, getmaxy(state.main_area) - LIST_TOP - 1);
}

void UIManager::setStatus(const std::string &message)
{
  state.status_message = message;
  state.status_expiry = std::time(nullptr) + 4;
}

std::string UIManager::selectedPath() const
{
  if (state.search_results_active)
  {
    if (state.search_view.selected < 0 || state.search_view.selected >= static_cast<int>(state.search_hits.size()))
      return {};
    return state.search_hits[state.search_view.selected].path;
  }

  if (state.browser.selected < 0 || state.browser.selected >= static_cast<int>(state.items.size()))
    return {};
  return state.source->join(state.current_directory, state.items[state.browser.selected].name);
}

// ---------------------------------------------------------------------------
// Directory listing
// ---------------------------------------------------------------------------

void UIManager::refreshListing(bool force)
{
  // The old code re-read the directory on every single frame because the
  // guard was `dir != last_listed || current_tab == directory`, and the second
  // clause is always true inside the directory tab. Cache on the path, with an
  // explicit validity flag so the remote root ("") isn't mistaken for "already
  // listed" on the very first frame.
  if (!force && state.listing_valid && state.current_directory == state.last_listed_directory)
    return;

  if (!state.source->list(state.current_directory, state.items))
  {
    setStatus("Cannot read " + (state.current_directory.empty() ? state.source->describe() : state.current_directory));
    state.items.clear();
  }

  state.last_listed_directory = state.current_directory;
  state.listing_valid = true;
  state.browser.clamp(static_cast<int>(state.items.size()), listHeight());
}

void UIManager::openParentDirectory()
{
  if (state.search_results_active)
  {
    state.search_results_active = false;
    return;
  }

  if (state.source->isRoot(state.current_directory))
    return; // already at the top of this source

  const std::string child = state.source->baseName(state.current_directory);
  state.current_directory = state.source->parent(state.current_directory);
  refreshListing(true);

  // Land the cursor on the directory we just left; walking up a tree and
  // losing your place is the single most annoying browser behaviour.
  state.browser.reset();
  for (size_t i = 0; i < state.items.size(); ++i)
  {
    if (state.items[i].is_dir && state.items[i].name == child)
    {
      state.browser.jump(static_cast<int>(i), static_cast<int>(state.items.size()));
      break;
    }
  }
}

void UIManager::runSearch(const std::string &query)
{
  state.search_hits.clear();
  state.search_query = query;
  state.search_root = state.current_directory;
  state.search_view.reset();

  if (query.empty())
  {
    state.search_results_active = false;
    return;
  }

  constexpr size_t kMaxHits = 2000;
  std::vector<std::pair<std::string, bool>> hits;
  if (!state.source->search(state.search_root, query, hits, kMaxHits))
  {
    setStatus("Search failed");
    return;
  }

  const size_t prefix = state.search_root.empty() ? 0 : state.search_root.size() + 1;
  state.search_hits.reserve(hits.size());
  for (auto &[path, is_dir] : hits)
  {
    SearchHit hit;
    hit.path = path;
    // Display relative to where the search started, so the useful part of a
    // deep path is visible instead of a wall of shared prefix.
    hit.display = path.size() > prefix ? path.substr(prefix) : state.source->baseName(path);
    hit.is_dir = is_dir;
    state.search_hits.push_back(std::move(hit));
  }

  state.search_results_active = true;
  setStatus(std::to_string(state.search_hits.size()) + " match(es) for \"" + query + "\"");
}

// ---------------------------------------------------------------------------
// Queueing
// ---------------------------------------------------------------------------

void UIManager::enqueuePath(const std::string &path, bool recursive, std::vector<SongMetadata> &out)
{
  if (state.source->isDirectory(path))
  {
    // A folder is an album: collect its files in listing order, then recurse
    // into sub-directories (multi-disc sets) in the same order.
    std::vector<Util::DirEntry> entries;
    if (!state.source->list(path, entries))
      return;

    for (const auto &entry : entries)
      if (!entry.is_dir && Util::isSupportedAudio(entry.name))
        out.push_back(state.source->metadata(state.source->join(path, entry.name)));

    if (recursive)
      for (const auto &entry : entries)
        if (entry.is_dir)
          enqueuePath(state.source->join(path, entry.name), true, out);
    return;
  }

  const std::string name = state.source->baseName(path);
  if (Util::isPlaylistFile(name))
  {
    // Playlists are read on the client even for a remote source: their
    // entries are absolute local paths, which only make sense here.
    std::vector<Playlist::Entry> entries;
    if (Playlist::load(path, entries))
      for (const auto &entry : entries)
        out.push_back(Library::loadSongMetadata(entry.path, false));
    return;
  }

  if (Util::isSupportedAudio(name))
    out.push_back(state.source->metadata(path));
}

void UIManager::startQueued(std::vector<SongMetadata> songs, bool replace_queue)
{
  if (songs.empty())
  {
    setStatus("Nothing playable there");
    return;
  }

  const size_t count = songs.size();
  if (replace_queue)
  {
    state.player.replaceQueue(std::move(songs), 0);
    setStatus("Playing " + std::to_string(count) + " track(s)");
  }
  else
  {
    const bool was_empty = state.player.isEmpty();
    state.player.queueSongs(std::move(songs));
    if (was_empty)
      state.player.playIndex(0);
    setStatus("Queued " + std::to_string(count) + " track(s)");
  }
  state.queue.clamp(static_cast<int>(state.player.getQueueSize()), listHeight());
}

void UIManager::enqueueSelection(bool replace_queue)
{
  const std::string path = selectedPath();
  if (path.empty())
    return;

  std::vector<SongMetadata> songs;
  enqueuePath(path, true, songs);
  startQueued(std::move(songs), replace_queue);
}

void UIManager::enterSelection()
{
  if (state.search_results_active)
  {
    if (state.search_view.selected < 0 || state.search_view.selected >= static_cast<int>(state.search_hits.size()))
      return;

    const SearchHit &hit = state.search_hits[state.search_view.selected];
    if (hit.is_dir)
    {
      state.current_directory = hit.path;
      state.search_results_active = false;
      state.browser.reset();
      refreshListing(true);
      return;
    }
    enqueueSelection(false);
    return;
  }

  if (state.browser.selected < 0 || state.browser.selected >= static_cast<int>(state.items.size()))
    return;

  const Util::DirEntry &entry = state.items[state.browser.selected];
  if (entry.is_dir)
  {
    state.current_directory = state.source->join(state.current_directory, entry.name);
    state.browser.reset();
    refreshListing(true);
    return;
  }

  enqueueSelection(false);
}

// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------

void UIManager::rebuildLibraryRows()
{
  state.library_rows.clear();
  if (state.album_expanded.size() != state.albums.size())
    state.album_expanded.assign(state.albums.size(), false);

  for (size_t a = 0; a < state.albums.size(); ++a)
  {
    state.library_rows.push_back({static_cast<int>(a), -1});
    if (!state.album_expanded[a])
      continue;
    for (size_t t = 0; t < state.albums[a].tracks.size(); ++t)
      state.library_rows.push_back({static_cast<int>(a), static_cast<int>(t)});
  }

  state.library.clamp(static_cast<int>(state.library_rows.size()), listHeight());
}

// ---------------------------------------------------------------------------
// Playlists
// ---------------------------------------------------------------------------

void UIManager::refreshPlaylists()
{
  // Keep the cursor on the same playlist across a reload; the list is
  // re-read after every create/rename/delete/add and jumping back to the
  // top each time makes building a playlist miserable.
  const std::string keep = state.playlists.selected >= 0 &&
                                   state.playlists.selected < static_cast<int>(state.playlist_names.size())
                               ? state.playlist_names[state.playlists.selected]
                               : std::string();

  state.playlist_names = Playlist::listSaved();
  state.previewed_playlist.clear();
  state.playlist_preview.clear();
  state.playlist_entries.reset();

  if (!keep.empty())
  {
    const auto it = std::find(state.playlist_names.begin(), state.playlist_names.end(), keep);
    if (it != state.playlist_names.end())
      state.playlists.selected = static_cast<int>(it - state.playlist_names.begin());
  }
  state.playlists.clamp(static_cast<int>(state.playlist_names.size()), listHeight());
}

void UIManager::loadPlaylistPreview()
{
  if (state.playlists.selected < 0 || state.playlists.selected >= static_cast<int>(state.playlist_names.size()))
  {
    state.playlist_preview.clear();
    state.previewed_playlist.clear();
    return;
  }

  const std::string &name = state.playlist_names[state.playlists.selected];
  if (name == state.previewed_playlist)
    return; // only re-read when the selection actually moves
  state.playlist_preview.clear();
  Playlist::loadNamed(name, state.playlist_preview);
  state.previewed_playlist = name;
  state.playlist_entries.reset();
}

void UIManager::playPlaylist(const std::string &name, bool replace_queue)
{
  std::vector<Playlist::Entry> entries;
  if (!Playlist::loadNamed(name, entries))
  {
    setStatus("Could not read playlist " + name);
    return;
  }

  const size_t missing = Playlist::pruneMissing(entries);
  std::vector<SongMetadata> songs;
  songs.reserve(entries.size());
  for (const auto &entry : entries)
    songs.push_back(Library::loadSongMetadata(entry.path, false));

  startQueued(std::move(songs), replace_queue);
  if (missing > 0)
    setStatus(std::to_string(missing) + " missing file(s) skipped in " + name);
}

void UIManager::savePlaylistFromQueue(const std::string &name)
{
  const auto &queue = state.player.getQueue();
  if (queue.empty())
  {
    setStatus("Queue is empty, nothing to save");
    return;
  }

  std::vector<Playlist::Entry> entries;
  entries.reserve(queue.size());
  for (const auto &song : queue)
    entries.push_back({song.song_path, song.song_name, song.duration_seconds});

  if (Playlist::saveNamed(name, entries))
  {
    refreshPlaylists();
    setStatus("Saved playlist \"" + name + "\" (" + std::to_string(entries.size()) + " tracks)");
  }
  else
  {
    setStatus("Failed to save playlist \"" + name + "\"");
  }
}

bool UIManager::createPlaylist(const std::string &name)
{
  if (name.empty())
    return false;

  const std::filesystem::path file = Playlist::pathForName(name);
  std::error_code ec;
  if (std::filesystem::exists(file, ec))
  {
    setStatus("Playlist \"" + name + "\" already exists");
    return false;
  }

  if (!Playlist::saveNamed(name, {}))
  {
    setStatus("Could not create \"" + name + "\"");
    return false;
  }

  refreshPlaylists();
  const auto it = std::find(state.playlist_names.begin(), state.playlist_names.end(), name);
  if (it != state.playlist_names.end())
    state.playlists.jump(static_cast<int>(it - state.playlist_names.begin()),
                         static_cast<int>(state.playlist_names.size()));
  setStatus("Created empty playlist \"" + name + "\"");
  return true;
}

/// Collect whatever the active tab has highlighted, ready to be filed away.
std::vector<SongMetadata> UIManager::selectionForPlaylist()
{
  std::vector<SongMetadata> songs;

  switch (state.current_tab)
  {
  case Tab::directory: {
    const std::string path = selectedPath();
    if (!path.empty())
      enqueuePath(path, true, songs);
    break;
  }
  case Tab::library: {
    if (state.library.selected < 0 || state.library.selected >= static_cast<int>(state.library_rows.size()))
      break;
    const LibraryRow row = state.library_rows[state.library.selected];
    Library::Album &album = state.albums[row.album_index];
    state.source->ensureTracks(static_cast<size_t>(row.album_index), album);
    if (row.track_index < 0)
      songs = state.source->albumToQueue(album);
    else if (row.track_index < static_cast<int>(album.tracks.size()))
      songs.push_back(state.source->metadata(album.tracks[row.track_index].path));
    break;
  }
  case Tab::queue: {
    const auto &queue = state.player.getQueue();
    if (state.queue.selected >= 0 && state.queue.selected < static_cast<int>(queue.size()))
      songs.push_back(queue[state.queue.selected]);
    break;
  }
  case Tab::home:
  case Tab::playlists:
  case Tab::help:
    // The playing track is the only sensible selection outside a browser.
    if (const SongMetadata *current = state.player.getCurrentSong())
      songs.push_back(*current);
    break;
  }

  return songs;
}

void UIManager::beginAddToPlaylist(std::vector<SongMetadata> songs)
{
  if (songs.empty())
  {
    setStatus("Nothing to add");
    return;
  }

  state.pending_add = std::move(songs);
  state.playlist_names = Playlist::listSaved();
  state.picker.reset();
  state.input_mode = InputMode::PickPlaylist;
}

void UIManager::addPendingToPlaylist(const std::string &name)
{
  if (state.pending_add.empty())
    return;

  std::vector<Playlist::Entry> entries;
  Playlist::loadNamed(name, entries); // absent file simply starts empty

  const size_t added = state.pending_add.size();
  entries.reserve(entries.size() + added);
  for (const auto &song : state.pending_add)
    entries.push_back({song.song_path, song.song_name, song.duration_seconds});

  if (!Playlist::saveNamed(name, entries))
  {
    // Hold on to the selection and reopen the picker: a full disk or a
    // read-only playlist directory shouldn't silently eat the tracks the
    // user just gathered, and dropping to Normal mode would strand them.
    setStatus("Could not write \"" + name + "\" - pick another destination");
    state.playlist_names = Playlist::listSaved();
    state.input_mode = InputMode::PickPlaylist;
    return;
  }
  state.pending_add.clear();

  refreshPlaylists();
  const auto it = std::find(state.playlist_names.begin(), state.playlist_names.end(), name);
  if (it != state.playlist_names.end())
    state.playlists.jump(static_cast<int>(it - state.playlist_names.begin()),
                         static_cast<int>(state.playlist_names.size()));
  setStatus("Added " + std::to_string(added) + " track(s) to \"" + name + "\"");
}

/// Persist in-place edits (reorder / remove) made in the Playlists tab.
void UIManager::commitPlaylistEdits()
{
  if (state.previewed_playlist.empty())
    return;
  if (!Playlist::saveNamed(state.previewed_playlist, state.playlist_preview))
    setStatus("Could not save \"" + state.previewed_playlist + "\"");
}

// ---------------------------------------------------------------------------
// Rendering: header / footer
// ---------------------------------------------------------------------------

void UIManager::updateHeader()
{
  werase(state.header);
  box(state.header, 0, 0);

  const auto now = std::time(nullptr);
  state.last_header_clock_update = now;
  state.last_tab = state.current_tab;

  auto *tm = std::localtime(&now);
  std::ostringstream oss;
  oss << std::put_time(tm, "%H:%M:%S");

  static const char *kTabNames[TAB_COUNT] = {"Home", "Library", "Directory", "Queue", "Playlists", "Help"};
  int x_pos = 2;
  for (int i = 0; i < TAB_COUNT; i++)
  {
    const bool active = static_cast<Tab>(i) == state.current_tab;
    if (active)
      wattron(state.header, A_REVERSE);
    mvwprintw(state.header, 1, x_pos, " %d %s ", i + 1, kTabNames[i]);
    if (active)
      wattroff(state.header, A_REVERSE);
    x_pos += static_cast<int>(std::string(kTabNames[i]).length()) + 5;
  }

  const std::string clock = oss.str();
  const int clock_x = state.max_cols - static_cast<int>(clock.size()) - 3;
  if (clock_x > x_pos)
    mvwprintw(state.header, 1, clock_x, "%s", clock.c_str());

  wnoutrefresh(state.header);
}

void UIManager::updateFooter()
{
  werase(state.footer);
  box(state.footer, 0, 0);

  const int inner = state.max_cols - 4;

  // PickPlaylist has no text entry: it draws its own modal list over the main
  // area, so the footer must keep showing transport state and the status line
  // (which is where a failed write reports itself).
  if (state.input_mode != InputMode::Normal && state.input_mode != InputMode::PickPlaylist)
  {
    printClipped(state.footer, 1, 2, inner, state.prompt_label + state.input_buffer + "_");
    wnoutrefresh(state.footer);
    return;
  }

  const auto *current = state.player.getCurrentSong();
  std::string left;
  if (current)
  {
    const char *marker = state.player.isPlaying() ? "\u25B6" : "\u2016";
    left = std::string(marker) + " " + current->artist_name + " - " + current->song_name;
  }
  else
  {
    left = "Ready - press <Enter> on a track in the Directory tab";
  }

  // Right side: time, volume, shuffle/repeat. Reserve it first so a long
  // title is clipped instead of overwriting the transport state.
  std::ostringstream right;
  if (current)
  {
    right << Util::formatDuration(state.player.getCurrentPositionSeconds()) << " / "
          << Util::formatDuration(state.player.getSongLengthSeconds()) << "  ";
  }
  right << "vol " << static_cast<int>(state.player.getVolume() * 100.0f + 0.5f) << "%"
        << "  shuf " << (state.player.isShuffle() ? "on" : "off") << "  rep " << repeatLabel(state.player.getRepeat());

  const std::string right_str = right.str();
  const int right_x = state.max_cols - static_cast<int>(right_str.size()) - 2;

  printClipped(state.footer, 1, 2, std::max(0, right_x - 3), left);
  if (right_x > 2)
    mvwprintw(state.footer, 1, right_x, "%s", right_str.c_str());

  if (!state.status_message.empty())
  {
    if (std::time(nullptr) > state.status_expiry)
      state.status_message.clear();
    else
      printClipped(state.footer, 2, 2, inner, state.status_message);
  }

  if (state.status_message.empty())
  {
    // A remote scan runs on the far end; STATUS is one cheap round trip and
    // is only issued while the footer actually has room to show it.
    const Source::ScanState scan = state.source->scanState();
    if (scan.scanning)
      printClipped(state.footer, 2, 2, inner,
                   "Indexing " + std::to_string(scan.done) + "/" + std::to_string(scan.total) + " - " +
                       state.source->describe());
    else if (state.source->isRemote() && !state.source->alive())
      printClipped(state.footer, 2, 2, inner, "Connection to " + state.source->describe() + " lost");
  }

  wnoutrefresh(state.footer);
}

// ---------------------------------------------------------------------------
// Rendering: screens
// ---------------------------------------------------------------------------

void UIManager::homeScreen()
{
  const int height = getmaxy(state.main_area);
  const int album_width = std::max(8, (state.max_cols / 2) - 4);
  const int album_height = std::max(4, height - 4);

  const auto *current = state.player.getCurrentSong();
  if (!current)
  {
    printClipped(state.main_area, 2, 2, state.max_cols - 4, "No track playing.");
    printClipped(state.main_area, 4, 2, state.max_cols - 4,
                 "2 Library  browse discovered albums    3 Directory  browse files");
    printClipped(state.main_area, 5, 2, state.max_cols - 4,
                 "5 Playlists  saved playlists           6 Help       all keybinds");
    return;
  }

  // Regenerate art when the track changes, the pane resizes, or the render
  // mode is toggled (which clears cached_song_path).
  if (state.cached_song_path != current->song_path || state.cached_art_width != album_width ||
      state.cached_art_height != album_height)
  {
    state.cached_song_path = current->song_path;
    state.cached_art_width = album_width;
    state.cached_art_height = album_height;

    // Cover art is loaded on demand rather than stored for every queue entry:
    // a queued album would otherwise hold one full RGBA bitmap per track.
    Art::ImageData image = current->cached_image;
    if (image.pixels.empty())
      state.source->coverArt(current->song_path, image);

    if (!image.pixels.empty())
      state.current_art = Art::Generate(image, state.art_color_mode, state.art_render_mode, album_width, album_height);
    else
      state.current_art = Art::AsciiCanvas();

    // Feed the art's palette to the visualizer so the EQ renders in the
    // album's colors. Mirrors the once-per-song cadence of art generation.
    state.viz_state.has_dynamic_palette = state.current_art.has_palette;
    if (state.current_art.has_palette)
      for (int i = 0; i < 16; ++i)
        state.viz_state.dynamic_palette[i] = state.current_art.palette[i];
  }

  if (!state.current_art.cells.empty())
  {
    int album_y = 1 + (album_height - state.current_art.height) / 2;
    album_y = std::max(album_y, 1);
    drawAsciiArt(state.main_area, album_y, 2, state.current_art);
  }
  else
  {
    printClipped(state.main_area, 2, 2, album_width, "[no cover art]");
  }

  const int info_x = album_width + 5;
  const int info_w = std::max(0, state.max_cols - info_x - 2);
  int y = 2;

  wattron(state.main_area, A_BOLD);
  printClipped(state.main_area, y, info_x, info_w, current->song_name);
  wattroff(state.main_area, A_BOLD);
  printClipped(state.main_area, y + 1, info_x, info_w, current->artist_name);
  if (!current->album_name.empty())
    printClipped(state.main_area, y + 2, info_x, info_w, current->album_name);

  const int pos = state.player.getCurrentPositionSeconds();
  const int total = state.player.getSongLengthSeconds();
  const int percent = state.player.getProgressPercent();

  const int bar_width = std::max(0, info_w);
  const int filled = (bar_width * percent) / 100;
  std::string bar;
  bar.reserve(static_cast<size_t>(bar_width) * 3);
  for (int i = 0; i < bar_width; ++i)
    bar += (i < filled) ? "\u2588" : "\u2591";

  printClipped(state.main_area, y + 4, info_x, info_w, bar);
  printClipped(state.main_area, y + 5, info_x, info_w,
               Util::formatDuration(pos) + " / " + Util::formatDuration(total));

  const int q_index = state.player.getCurrentIndex();
  const size_t q_size = state.player.getQueueSize();
  printClipped(state.main_area, y + 6, info_x, info_w,
               "Track " + std::to_string(q_index + 1) + " of " + std::to_string(q_size));

  // Visualizer / EQ: below the timer, right of the album art.
  RingBuffer *ring = state.player.getRingBuffer();
  if (ring)
  {
    const int viz_y = y + 8;
    const int viz_h = height - viz_y - 2;
    if (info_w > 0 && viz_h > 0)
      Visualizer::render(state.main_area, viz_y, info_x, info_w, viz_h, state.viz_state, *ring,
                         state.player.getSampleRate());
  }
}

void UIManager::libraryScreen()
{
  const Source::ScanState scan = state.source->scanState();
  const size_t scanned_albums = static_cast<size_t>(scan.albums);
  if (state.library_dirty || scanned_albums != state.last_album_count)
  {
    state.source->albums(state.albums);
    state.last_album_count = scanned_albums;
    state.library_dirty = false;
    state.album_expanded.assign(state.albums.size(), false);
    rebuildLibraryRows();
  }

  std::string title = "Albums (" + std::to_string(state.albums.size()) + ") - " + state.source->describe();
  if (scan.scanning)
    title += "  indexing " + std::to_string(scan.done) + "/" + std::to_string(scan.total);
  printClipped(state.main_area, 1, 2, state.max_cols - 4, title);

  if (state.library_rows.empty())
  {
    printClipped(state.main_area, LIST_TOP, 2, state.max_cols - 4,
                 scan.scanning ? "Indexing..." : "No albums found. Press 'R' to rescan.");
    return;
  }

  const int height = listHeight();
  state.library.clamp(static_cast<int>(state.library_rows.size()), height);

  const int width = state.max_cols - 4;
  for (int row = 0; row < height; ++row)
  {
    const int idx = state.library.scroll + row;
    if (idx >= static_cast<int>(state.library_rows.size()))
      break;

    const LibraryRow &lr = state.library_rows[idx];
    const Library::Album &album = state.albums[lr.album_index];
    std::string text;

    if (lr.track_index < 0)
    {
      const char *marker = state.album_expanded[lr.album_index] ? "v" : ">";
      text = std::string(marker) + " " + album.artist + " - " + album.title;
      if (album.year > 0)
        text += " (" + std::to_string(album.year) + ")";
      text += "  [" + std::to_string(album.track_count) + " tracks, " +
              Util::formatDuration(album.total_seconds) + "]";
    }
    else
    {
      const Library::Track &track = album.tracks[lr.track_index];
      std::ostringstream num;
      num << std::setw(2) << std::setfill('0') << (track.track_number > 0 ? track.track_number : lr.track_index + 1);
      text = "    " + num.str() + "  " + track.title + "  " + Util::formatDuration(track.duration_seconds);
    }

    printRow(state.main_area, LIST_TOP + row, 2, width, text, idx == state.library.selected);
  }
}

void UIManager::directoryScreen()
{
  refreshListing(false);

  const int height = listHeight();
  const int width = state.max_cols - 4;

  if (state.search_results_active)
  {
    printClipped(state.main_area, 1, 2, width,
                 "Search \"" + state.search_query + "\" in " + state.search_root + "  (<Esc> to exit)");

    state.search_view.clamp(static_cast<int>(state.search_hits.size()), height);
    if (state.search_hits.empty())
      printClipped(state.main_area, LIST_TOP, 2, width, "No matches.");

    for (int row = 0; row < height; ++row)
    {
      const int idx = state.search_view.scroll + row;
      if (idx >= static_cast<int>(state.search_hits.size()))
        break;
      const SearchHit &hit = state.search_hits[idx];
      printRow(state.main_area, LIST_TOP + row, 2, width, (hit.is_dir ? hit.display + "/" : hit.display),
               idx == state.search_view.selected);
    }
    return;
  }

  // At a source's root the path can be empty (remote), so name the source.
  const std::string where =
      state.source->isRoot(state.current_directory) ? state.source->describe() : state.current_directory;
  printClipped(state.main_area, 1, 2, width, "Directory: " + where);

  state.browser.clamp(static_cast<int>(state.items.size()), height);
  if (state.items.empty())
  {
    printClipped(state.main_area, LIST_TOP, 2, width, "(no playable files here)");
    return;
  }

  for (int row = 0; row < height; ++row)
  {
    const int idx = state.browser.scroll + row;
    if (idx >= static_cast<int>(state.items.size()))
      break;

    const Util::DirEntry &entry = state.items[idx];
    const std::string text = entry.is_dir ? entry.name + "/" : entry.name;
    printRow(state.main_area, LIST_TOP + row, 2, width, text, idx == state.browser.selected);
  }

  if (state.items.size() > static_cast<size_t>(height))
  {
    const std::string pos = std::to_string(state.browser.selected + 1) + "/" + std::to_string(state.items.size());
    mvwprintw(state.main_area, 1, std::max(2, state.max_cols - static_cast<int>(pos.size()) - 3), "%s", pos.c_str());
  }
}

void UIManager::queueScreen()
{
  const auto &queue = state.player.getQueue();
  const int height = listHeight();
  const int width = state.max_cols - 4;

  printClipped(state.main_area, 1, 2, width, "Queue (" + std::to_string(queue.size()) + " tracks)");

  if (queue.empty())
  {
    printClipped(state.main_area, LIST_TOP, 2, width, "Queue empty.");
    return;
  }

  state.queue.clamp(static_cast<int>(queue.size()), height);
  const int playing = state.player.getCurrentIndex();

  for (int row = 0; row < height; ++row)
  {
    const int idx = state.queue.scroll + row;
    if (idx >= static_cast<int>(queue.size()))
      break;

    const SongMetadata &song = queue[idx];
    std::ostringstream line;
    line << (idx == playing ? "> " : "  ") << std::setw(3) << (idx + 1) << ". " << song.artist_name << " - "
         << song.song_name;
    if (song.duration_seconds > 0)
      line << "  [" << Util::formatDuration(song.duration_seconds) << "]";

    printRow(state.main_area, LIST_TOP + row, 2, width, line.str(), idx == state.queue.selected);
  }
}

void UIManager::playlistScreen()
{
  const int height = listHeight();
  const int split = std::max(20, state.max_cols / 3);
  const int left_w = split - 3;
  const int right_x = split + 2;
  const int right_w = std::max(0, state.max_cols - right_x - 2);
  const bool entries_focused = state.playlist_focus == PlaylistPane::Entries;

  printClipped(state.main_area, 1, 2, left_w, "Playlists (" + std::to_string(state.playlist_names.size()) + ")");

  if (state.playlist_names.empty())
  {
    printClipped(state.main_area, LIST_TOP, 2, state.max_cols - 4,
                 "No saved playlists yet. Press 'a' to create one, or 'w' in the Queue tab");
    printClipped(state.main_area, LIST_TOP + 1, 2, state.max_cols - 4,
                 "to save the current queue. 'A' in any browser files tracks into one.");
    return;
  }

  state.playlists.clamp(static_cast<int>(state.playlist_names.size()), height);
  loadPlaylistPreview();

  for (int row = 0; row < height; ++row)
  {
    const int idx = state.playlists.scroll + row;
    if (idx >= static_cast<int>(state.playlist_names.size()))
      break;
    // Only the focused pane shows a cursor, so it is always obvious which
    // list j/k and 'd' are about to act on.
    printRow(state.main_area, LIST_TOP + row, 2, left_w, state.playlist_names[idx],
             idx == state.playlists.selected && !entries_focused);
  }

  for (int row = 0; row < height; ++row)
    mvwaddch(state.main_area, LIST_TOP + row, split, ACS_VLINE);

  std::string right_title = state.previewed_playlist + " (" + std::to_string(state.playlist_preview.size()) + ")";
  right_title += entries_focused ? "  <Left> back" : "  <Right> edit";
  printClipped(state.main_area, 1, right_x, right_w, right_title);

  if (state.playlist_preview.empty())
  {
    printClipped(state.main_area, LIST_TOP, right_x, right_w, "(empty)");
    return;
  }

  state.playlist_entries.clamp(static_cast<int>(state.playlist_preview.size()), height);

  for (int row = 0; row < height; ++row)
  {
    const int idx = state.playlist_entries.scroll + row;
    if (idx >= static_cast<int>(state.playlist_preview.size()))
      break;
    const Playlist::Entry &entry = state.playlist_preview[idx];
    std::string label = entry.title.empty() ? std::filesystem::path(entry.path).filename().string() : entry.title;
    label = std::to_string(idx + 1) + ". " + label;
    if (entry.duration_seconds > 0)
      label += "  [" + Util::formatDuration(entry.duration_seconds) + "]";
    printRow(state.main_area, LIST_TOP + row, right_x, right_w, label,
             entries_focused && idx == state.playlist_entries.selected);
  }
}

/// Modal list of playlists to file the pending tracks into, drawn over the
/// current tab. Row 0 is always "create a new one".
void UIManager::drawPlaylistPicker()
{
  const int rows = static_cast<int>(state.playlist_names.size()) + 1;
  const int body = getmaxy(state.main_area);
  const int win_h = std::min(body - 2, std::max(5, rows + 4));
  const int win_w = std::min(state.max_cols - 6, 64);
  const int top = std::max(1, (body - win_h) / 2);
  const int left = std::max(2, (state.max_cols - win_w) / 2);
  const int inner_w = win_w - 4;
  const int visible = win_h - 4;

  for (int y = 0; y < win_h; ++y)
    for (int x = 0; x < win_w; ++x)
      mvwaddch(state.main_area, top + y, left + x, ' ');

  // Manual border: this is a region inside main_area, not its own WINDOW.
  mvwaddch(state.main_area, top, left, ACS_ULCORNER);
  mvwaddch(state.main_area, top, left + win_w - 1, ACS_URCORNER);
  mvwaddch(state.main_area, top + win_h - 1, left, ACS_LLCORNER);
  mvwaddch(state.main_area, top + win_h - 1, left + win_w - 1, ACS_LRCORNER);
  for (int x = 1; x < win_w - 1; ++x)
  {
    mvwaddch(state.main_area, top, left + x, ACS_HLINE);
    mvwaddch(state.main_area, top + win_h - 1, left + x, ACS_HLINE);
  }
  for (int y = 1; y < win_h - 1; ++y)
  {
    mvwaddch(state.main_area, top + y, left, ACS_VLINE);
    mvwaddch(state.main_area, top + y, left + win_w - 1, ACS_VLINE);
  }

  printClipped(state.main_area, top + 1, left + 2, inner_w,
               "Add " + std::to_string(state.pending_add.size()) + " track(s) to:");

  state.picker.clamp(rows, visible);
  for (int row = 0; row < visible; ++row)
  {
    const int idx = state.picker.scroll + row;
    if (idx >= rows)
      break;
    const std::string label = idx == 0 ? "[+ new playlist]" : state.playlist_names[idx - 1];
    printRow(state.main_area, top + 2 + row, left + 2, inner_w, label, idx == state.picker.selected);
  }

  printClipped(state.main_area, top + win_h - 2, left + 2, inner_w, "<Enter> add   <Esc> cancel");
}

namespace
{

// Kept at file scope so the key handler can scroll it without duplicating the
// line count. The help outgrew a single screen the moment playlists landed.
const char *const kHelpLines[] = {
    "GLOBAL",
    "  q                       quit            1-6 / Tab / h l    switch tabs",
    "  p or <Space>            play / pause    ] [                next / prev track",
    "  , .                     seek -5s / +5s  9 0                volume down / up",
    "  s                       shuffle         r                  repeat off/all/one",
    "  A                       add the selection to a playlist (picker)",
    "  a                       art palette (ansi / grayscale)",
    "  z                       art style (block / detailed)",
    "  v                       visualizer style (block / ansi / braille / spectrogram)",
    "  c                       visualizer colors (album art / fixed)",
    "",
    "LISTS (all tabs)",
    "  j k / <Down> <Up>       move (wraps at both ends)",
    "  Ctrl-D / Ctrl-U         half page down / up",
    "  Ctrl-F / Ctrl-B         full page down / up",
    "  g / G                   jump to top / bottom",
    "",
    "DIRECTORY",
    "  <Enter>                 open folder, or queue the track",
    "  e                       queue folder/track (folders load recursively)",
    "  P                       play folder/track now (replaces the queue)",
    "  A                       add folder/track to a playlist",
    "  - / <Backspace>         go up a directory",
    "  /                       recursive search below this folder, <Esc> clears",
    "  R                       re-read this directory",
    "",
    "LIBRARY",
    "  <Enter>                 expand / collapse album, or play a track",
    "  P                       play album now      e    queue album",
    "  A                       add album/track to a playlist",
    "  R                       rescan the library (forces a fresh tag read)",
    "",
    "QUEUE",
    "  <Enter>                 play the highlighted track",
    "  d                       remove track        X    clear queue (confirm)",
    "  J / K                   move track down / up",
    "  A                       add the highlighted track to a playlist",
    "  w                       save the whole queue as a new playlist",
    "",
    "PLAYLISTS",
    "  a                       create a new, empty playlist",
    "  <Enter>                 play playlist       e    append to queue",
    "  d                       delete (confirm)    R    rename",
    "  <Right>                 edit the track list; <Left> or <Esc> goes back",
    "    j k                   move through tracks",
    "    J / K                 reorder (saved immediately)",
    "    d                     remove the track from the playlist (confirm)",
    "    <Enter>               play the playlist from this track",
};
constexpr int kHelpCount = static_cast<int>(sizeof(kHelpLines) / sizeof(kHelpLines[0]));

} // namespace

void UIManager::helpScreen()
{
  const int width = state.max_cols - 4;
  const int height = listHeight();

  // The help has no cursor, so `selected` IS the scroll offset: j/k slide the
  // text rather than chasing an invisible highlight to the bottom first.
  const int max_scroll = std::max(0, kHelpCount - height);
  state.help.selected = std::clamp(state.help.selected, 0, max_scroll);
  state.help.scroll = state.help.selected;

  std::string title = "Help - config: " + Config::configPath().string();
  if (max_scroll > 0)
    title += "   (j/k to scroll)";
  printClipped(state.main_area, 1, 2, width, title);

  for (int row = 0; row < height; ++row)
  {
    const int idx = state.help.scroll + row;
    if (idx >= kHelpCount)
      break;
    printClipped(state.main_area, LIST_TOP + row, 2, width, kHelpLines[idx]);
  }
}

void UIManager::updateMainArea()
{
  werase(state.main_area);
  box(state.main_area, 0, 0);

  switch (state.current_tab)
  {
  case Tab::library:
    libraryScreen();
    break;
  case Tab::directory:
    directoryScreen();
    break;
  case Tab::queue:
    queueScreen();
    break;
  case Tab::playlists:
    playlistScreen();
    break;
  case Tab::help:
    helpScreen();
    break;
  case Tab::home:
    homeScreen();
    break;
  }

  if (state.input_mode == InputMode::PickPlaylist)
    drawPlaylistPicker();

  wnoutrefresh(state.main_area);
}

void UIManager::render()
{
  if (state.max_rows < MIN_ROWS || state.max_cols < MIN_COLS)
  {
    werase(stdscr);
    mvprintw(0, 0, "Terminal too small (%dx%d); need at least %dx%d", state.max_cols, state.max_rows, MIN_COLS,
             MIN_ROWS);
    wnoutrefresh(stdscr);
    doupdate();
    return;
  }

  updateHeader();
  updateMainArea();
  updateFooter();
  doupdate(); // one flush per frame: no mid-frame tearing between panes
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void UIManager::submitPrompt()
{
  const InputMode mode = state.input_mode;
  const std::string text = state.input_buffer;
  state.input_mode = InputMode::Normal;
  state.input_buffer.clear();
  state.prompt_label.clear();

  switch (mode)
  {
  case InputMode::Search:
    runSearch(text);
    break;
  case InputMode::SavePlaylist:
    if (!text.empty())
      savePlaylistFromQueue(text);
    break;
  case InputMode::RenamePlaylist:
    if (!text.empty() && state.playlists.selected < static_cast<int>(state.playlist_names.size()))
    {
      const std::string from = state.playlist_names[state.playlists.selected];
      if (Playlist::renameNamed(from, text))
      {
        refreshPlaylists();
        setStatus("Renamed \"" + from + "\" to \"" + text + "\"");
      }
      else
      {
        setStatus("Rename failed");
      }
    }
    break;
  case InputMode::NewPlaylist:
    createPlaylist(text);
    break;
  case InputMode::NamePlaylistForAdd:
    if (text.empty())
      state.pending_add.clear();
    else
      addPendingToPlaylist(text);
    break;
  case InputMode::PickPlaylist:
  case InputMode::Confirm:
  case InputMode::Normal:
    break;
  }
}

void UIManager::handlePromptKey(int ch)
{
  if (state.input_mode == InputMode::Confirm)
  {
    if (ch == 'y' || ch == 'Y')
    {
      switch (state.confirm_action)
      {
      case ConfirmAction::ClearQueue:
        state.player.clearQueue();
        state.queue.reset();
        state.cached_song_path.clear();
        setStatus("Queue cleared");
        break;
      case ConfirmAction::DeletePlaylist:
        if (state.playlists.selected < static_cast<int>(state.playlist_names.size()))
        {
          const std::string name = state.playlist_names[state.playlists.selected];
          setStatus(Playlist::removeNamed(name) ? "Deleted \"" + name + "\"" : "Delete failed");
          state.playlists.selected = std::max(0, state.playlists.selected - 1);
          refreshPlaylists();
          state.playlist_focus = PlaylistPane::List;
        }
        break;
      case ConfirmAction::RemovePlaylistEntry:
        if (state.playlist_entries.selected < static_cast<int>(state.playlist_preview.size()))
        {
          state.playlist_preview.erase(state.playlist_preview.begin() + state.playlist_entries.selected);
          commitPlaylistEdits();
          state.playlist_entries.clamp(static_cast<int>(state.playlist_preview.size()), listHeight());
          setStatus("Removed from \"" + state.previewed_playlist + "\"");
        }
        break;
      case ConfirmAction::None:
        break;
      }
    }
    state.confirm_action = ConfirmAction::None;
    state.input_mode = InputMode::Normal;
    state.prompt_label.clear();
    return;
  }

  switch (ch)
  {
  case '\n':
  case KEY_ENTER:
    submitPrompt();
    return;
  case 27: // ESC
    state.input_mode = InputMode::Normal;
    state.input_buffer.clear();
    state.prompt_label.clear();
    state.pending_add.clear();
    return;
  case KEY_BACKSPACE:
  case 127:
  case 8:
    if (!state.input_buffer.empty())
      state.input_buffer.pop_back();
    return;
  default:
    break;
  }

  if (ch >= 32 && ch < 127 && state.input_buffer.size() < 120)
    state.input_buffer.push_back(static_cast<char>(ch));
}

void UIManager::handlePickerKey(int ch)
{
  const int rows = static_cast<int>(state.playlist_names.size()) + 1; // +1 for "[+ new playlist]"

  if (handleListNavKey(ch, state.picker, rows, std::max(1, getmaxy(state.main_area) - 6)))
    return;

  switch (ch)
  {
  case 27: // ESC
    state.input_mode = InputMode::Normal;
    state.pending_add.clear();
    return;
  case '\n':
  case KEY_ENTER:
    if (state.picker.selected == 0)
    {
      // Chain into a name prompt; pending_add survives the mode switch.
      state.input_mode = InputMode::NamePlaylistForAdd;
      state.prompt_label = "new playlist name: ";
      state.input_buffer.clear();
    }
    else
    {
      const std::string name = state.playlist_names[state.picker.selected - 1];
      state.input_mode = InputMode::Normal;
      addPendingToPlaylist(name);
    }
    return;
  default:
    return;
  }
}

bool UIManager::handleListNavKey(int ch, ListView &view, int count, int height)
{
  const int half = std::max(1, height / 2);

  switch (ch)
  {
  case 'j':
  case KEY_DOWN:
    view.move(1, count);
    return true;
  case 'k':
  case KEY_UP:
    view.move(-1, count);
    return true;
  case 4: // Ctrl-D
    view.move(half, count, false);
    return true;
  case 21: // Ctrl-U
    view.move(-half, count, false);
    return true;
  case 6: // Ctrl-F
  case KEY_NPAGE:
    view.move(std::max(1, height), count, false);
    return true;
  case 2: // Ctrl-B
  case KEY_PPAGE:
    view.move(-std::max(1, height), count, false);
    return true;
  case 'g':
  case KEY_HOME:
    view.jump(0, count);
    return true;
  case 'G':
  case KEY_END:
    view.jump(count - 1, count);
    return true;
  default:
    return false;
  }
}

void UIManager::handleGlobalKey(int ch)
{
  switch (ch)
  {
  case KEY_LEFT:
  case 'h':
  case KEY_BTAB: {
    int current = static_cast<int>(state.current_tab);
    state.current_tab = static_cast<Tab>((current + TAB_COUNT - 1) % TAB_COUNT);
    return;
  }
  case KEY_RIGHT:
  case 'l':
  case '\t': {
    int current = static_cast<int>(state.current_tab);
    state.current_tab = static_cast<Tab>((current + 1) % TAB_COUNT);
    return;
  }
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
    state.current_tab = static_cast<Tab>(ch - '1');
    return;
  case '?':
    state.current_tab = Tab::help;
    return;

  case 'p':
  case ' ':
    state.player.pauseSong();
    return;
  case ']':
    state.player.nextSong();
    return;
  case '[':
    state.player.prevSong();
    return;
  case ',':
    state.player.seekRelative(-5);
    return;
  case '.':
    state.player.seekRelative(5);
    return;
  case '0':
    state.player.adjustVolume(0.05f);
    setStatus("Volume " + std::to_string(static_cast<int>(state.player.getVolume() * 100.0f + 0.5f)) + "%");
    return;
  case '9':
    state.player.adjustVolume(-0.05f);
    setStatus("Volume " + std::to_string(static_cast<int>(state.player.getVolume() * 100.0f + 0.5f)) + "%");
    return;
  case 's':
    state.player.toggleShuffle();
    setStatus(state.player.isShuffle() ? "Shuffle on" : "Shuffle off");
    return;
  case 'r':
    state.player.cycleRepeat();
    setStatus(std::string("Repeat ") + repeatLabel(state.player.getRepeat()));
    return;

  case 'a':
    state.art_color_mode =
        (state.art_color_mode == Art::ColorMode::ANSI_256) ? Art::ColorMode::GRAYSCALE : Art::ColorMode::ANSI_256;
    state.cached_song_path.clear(); // force art regen on next frame
    return;
  case 'z':
    state.art_render_mode =
        (state.art_render_mode == Art::RenderMode::BLOCK) ? Art::RenderMode::DETAILED : Art::RenderMode::BLOCK;
    state.cached_song_path.clear();
    return;
  case 'v':
    switch (state.viz_state.style)
    {
    case Visualizer::Style::BLOCK:
      state.viz_state.style = Visualizer::Style::ANSI_ART;
      break;
    case Visualizer::Style::ANSI_ART:
      state.viz_state.style = Visualizer::Style::BRAILLE;
      break;
    case Visualizer::Style::BRAILLE:
      state.viz_state.style = Visualizer::Style::SPECTROGRAM;
      break;
    case Visualizer::Style::SPECTROGRAM:
      state.viz_state.style = Visualizer::Style::BLOCK;
      break;
    }
    state.viz_state.analyzer.bar_count = 0; // force binning reconfigure
    setStatus("Visualizer: " + Visualizer::styleName(state.viz_state.style));
    return;
  case 'c':
    state.viz_state.use_dynamic_palette = !state.viz_state.use_dynamic_palette;
    setStatus(std::string("Visualizer palette: ") + (state.viz_state.use_dynamic_palette ? "album art" : "fixed"));
    return;
  default:
    return;
  }
}

bool UIManager::handleDirectoryKey(int ch)
{
  const int height = listHeight();
  ListView &view = state.search_results_active ? state.search_view : state.browser;
  const int count = state.search_results_active ? static_cast<int>(state.search_hits.size())
                                                : static_cast<int>(state.items.size());

  if (handleListNavKey(ch, view, count, height))
    return true;

  switch (ch)
  {
  case '\n':
  case KEY_ENTER:
    enterSelection();
    return true;
  case 'e':
    enqueueSelection(false);
    return true;
  case 'P':
    enqueueSelection(true);
    return true;
  case 'A':
    beginAddToPlaylist(selectionForPlaylist());
    return true;
  case '-':
  case KEY_BACKSPACE:
  case 127:
    openParentDirectory();
    return true;
  case 27: // ESC
    if (state.search_results_active)
      state.search_results_active = false;
    else
      openParentDirectory();
    return true;
  case '/':
    state.input_mode = InputMode::Search;
    state.prompt_label = "search: ";
    state.input_buffer.clear();
    return true;
  case 'R':
    refreshListing(true);
    setStatus("Reloaded " + state.current_directory);
    return true;
  default:
    return false;
  }
}

bool UIManager::handleLibraryKey(int ch)
{
  const int height = listHeight();
  const int count = static_cast<int>(state.library_rows.size());

  if (handleListNavKey(ch, state.library, count, height))
    return true;

  if (ch == 'R')
  {
    state.source->startScan(true);
    state.library_dirty = true;
    setStatus("Rescanning " + state.source->describe() + "...");
    return true;
  }

  if (count == 0 || state.library.selected >= count)
    return false;

  const LibraryRow row = state.library_rows[state.library.selected];
  Library::Album &album = state.albums[row.album_index];

  switch (ch)
  {
  case '\n':
  case KEY_ENTER:
    if (row.track_index < 0)
    {
      // Expanding is where a remote album pays for its track list; the list
      // view only needed the summary until now.
      if (!state.album_expanded[row.album_index] &&
          !state.source->ensureTracks(static_cast<size_t>(row.album_index), album))
      {
        setStatus("Could not read tracks for " + album.title);
        return true;
      }
      state.album_expanded[row.album_index] = !state.album_expanded[row.album_index];
      rebuildLibraryRows();
    }
    else
    {
      // Play the whole album but start on the chosen track.
      auto songs = state.source->albumToQueue(album);
      const int start = row.track_index;
      state.player.replaceQueue(std::move(songs), start);
      setStatus("Playing " + album.title);
    }
    return true;
  case 'P':
    state.source->ensureTracks(static_cast<size_t>(row.album_index), album);
    startQueued(state.source->albumToQueue(album), true);
    return true;
  case 'A':
    beginAddToPlaylist(selectionForPlaylist());
    return true;
  case 'e':
    if (row.track_index < 0)
    {
      state.source->ensureTracks(static_cast<size_t>(row.album_index), album);
      startQueued(state.source->albumToQueue(album), false);
    }
    else
    {
      std::vector<SongMetadata> one;
      one.push_back(state.source->metadata(album.tracks[row.track_index].path));
      startQueued(std::move(one), false);
    }
    return true;
  default:
    return false;
  }
}

bool UIManager::handleQueueKey(int ch)
{
  const int height = listHeight();
  const int count = static_cast<int>(state.player.getQueueSize());

  if (handleListNavKey(ch, state.queue, count, height))
    return true;

  switch (ch)
  {
  case '\n':
  case KEY_ENTER:
    if (count > 0)
      state.player.playIndex(state.queue.selected);
    return true;
  case 'd':
    if (count > 0 && state.player.removeAt(state.queue.selected))
    {
      state.queue.clamp(static_cast<int>(state.player.getQueueSize()), height);
      setStatus("Removed from queue");
    }
    return true;
  case 'A':
    beginAddToPlaylist(selectionForPlaylist());
    return true;
  case 'J':
    if (state.player.moveItem(state.queue.selected, state.queue.selected + 1))
      state.queue.move(1, count, false);
    return true;
  case 'K':
    if (state.player.moveItem(state.queue.selected, state.queue.selected - 1))
      state.queue.move(-1, count, false);
    return true;
  case 'X':
    if (count > 0)
    {
      state.input_mode = InputMode::Confirm;
      state.confirm_action = ConfirmAction::ClearQueue;
      state.prompt_label = "Clear the whole queue? (y/n) ";
    }
    return true;
  case 'w':
    if (count > 0)
    {
      state.input_mode = InputMode::SavePlaylist;
      state.prompt_label = "save playlist as: ";
      state.input_buffer.clear();
    }
    else
    {
      setStatus("Queue is empty, nothing to save");
    }
    return true;
  default:
    return false;
  }
}

bool UIManager::handlePlaylistKey(int ch)
{
  const int height = listHeight();
  const int count = static_cast<int>(state.playlist_names.size());

  // 'a' creates a playlist from nothing, so it must work with an empty list.
  if (ch == 'a')
  {
    state.input_mode = InputMode::NewPlaylist;
    state.prompt_label = "new playlist name: ";
    state.input_buffer.clear();
    return true;
  }
  if (ch == 'A')
  {
    beginAddToPlaylist(selectionForPlaylist());
    return true;
  }
  if (count == 0)
    return false;

  // The right pane is a real editor, not a preview: focus it to reorder or
  // drop tracks. Arrows move focus here; h/l stay global tab switching.
  if (state.playlist_focus == PlaylistPane::Entries)
  {
    const int entries = static_cast<int>(state.playlist_preview.size());

    if (ch == KEY_LEFT || ch == 27)
    {
      state.playlist_focus = PlaylistPane::List;
      return true;
    }
    if (handleListNavKey(ch, state.playlist_entries, entries, height))
      return true;
    if (entries == 0 || state.playlist_entries.selected >= entries)
      return false;

    switch (ch)
    {
    case '\n':
    case KEY_ENTER:
      // Play the playlist starting from the highlighted entry.
      playPlaylist(state.previewed_playlist, true);
      state.player.playIndex(state.playlist_entries.selected);
      return true;
    case 'd':
      state.input_mode = InputMode::Confirm;
      state.confirm_action = ConfirmAction::RemovePlaylistEntry;
      state.prompt_label = "Remove this track from the playlist? (y/n) ";
      return true;
    case 'J':
      if (state.playlist_entries.selected + 1 < entries)
      {
        std::swap(state.playlist_preview[state.playlist_entries.selected],
                  state.playlist_preview[state.playlist_entries.selected + 1]);
        state.playlist_entries.move(1, entries, false);
        commitPlaylistEdits();
      }
      return true;
    case 'K':
      if (state.playlist_entries.selected > 0)
      {
        std::swap(state.playlist_preview[state.playlist_entries.selected],
                  state.playlist_preview[state.playlist_entries.selected - 1]);
        state.playlist_entries.move(-1, entries, false);
        commitPlaylistEdits();
      }
      return true;
    default:
      return false;
    }
  }

  if (ch == KEY_RIGHT)
  {
    state.playlist_focus = PlaylistPane::Entries;
    state.playlist_entries.reset();
    return true;
  }
  if (handleListNavKey(ch, state.playlists, count, height))
    return true;
  if (state.playlists.selected >= count)
    return false;

  const std::string name = state.playlist_names[state.playlists.selected];

  switch (ch)
  {
  case '\n':
  case KEY_ENTER:
    playPlaylist(name, true);
    return true;
  case 'e':
    playPlaylist(name, false);
    return true;
  case 'd':
    state.input_mode = InputMode::Confirm;
    state.confirm_action = ConfirmAction::DeletePlaylist;
    state.prompt_label = "Delete playlist \"" + name + "\"? (y/n) ";
    return true;
  case 'R':
    state.input_mode = InputMode::RenamePlaylist;
    state.prompt_label = "rename to: ";
    state.input_buffer = name;
    return true;
  default:
    return false;
  }
}

void UIManager::handleKey(int ch)
{
  if (ch == KEY_RESIZE)
  {
    handleResize();
    return;
  }

  if (state.input_mode == InputMode::PickPlaylist)
  {
    handlePickerKey(ch);
    return;
  }
  if (state.input_mode != InputMode::Normal)
  {
    handlePromptKey(ch);
    return;
  }

  // Tab-local handlers run first and report whether they consumed the key, so
  // 'a', 'e', 'R' and friends can mean one thing inside a browser and another
  // globally without maintaining a hand-written list of claimed keys.
  bool consumed = false;
  switch (state.current_tab)
  {
  case Tab::directory:
    consumed = handleDirectoryKey(ch);
    break;
  case Tab::library:
    consumed = handleLibraryKey(ch);
    break;
  case Tab::queue:
    consumed = handleQueueKey(ch);
    break;
  case Tab::playlists:
    consumed = handlePlaylistKey(ch);
    break;
  case Tab::help: {
    const int height = listHeight();
    consumed = handleListNavKey(ch, state.help, std::max(0, kHelpCount - height) + 1, height);
    break;
  }
  case Tab::home:
    break;
  }

  // 'A' files the current selection away from any tab; on Home and Help that
  // means the playing track.
  if (!consumed && ch == 'A')
  {
    beginAddToPlaylist(selectionForPlaylist());
    consumed = true;
  }

  if (!consumed)
    handleGlobalKey(ch);
}

// ---------------------------------------------------------------------------
// Event loop
// ---------------------------------------------------------------------------

void UIManager::run()
{
  Util::debugPrint("Starting TUI event loop");

  const int frame_ms = std::max(1, 1000 / std::clamp(state.cfg.fps, 5, 120));
  // Blocking read with a frame-length timeout: the old loop spun with
  // nodelay + napms(5), burning a core to redraw 200x/second.
  keypad(stdscr, TRUE);
  timeout(frame_ms);

  bool running = true;
  while (running)
  {
    // One blocking read paced at the frame interval, then drain whatever is
    // already buffered. The drain MUST be non-blocking: timeout() applies to
    // every getch(), so a held key (repeating faster than the frame budget)
    // would keep the drain loop alive indefinitely and starve render().
    int ch = getch();
    if (ch != ERR)
    {
      timeout(0);
      do
      {
        if (ch == 'q' && state.input_mode == InputMode::Normal)
        {
          running = false;
          break;
        }
        handleKey(ch);
        ch = getch();
      } while (ch != ERR);
      timeout(frame_ms);
    }

    state.player.tick();
    render();
  }

  Util::debugPrint("User has quit TUI");
}
