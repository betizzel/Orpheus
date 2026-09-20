#pragma once

// direct includes
#include <ncurses.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include "art.hpp"
#include "config.hpp"
#include "library.hpp"
#include "player.hpp"
#include "playlist.hpp"
#include "source.hpp"
#include "util.hpp"
#include "visualizer.hpp"

inline constexpr int TAB_COUNT = 6;

enum class Tab
{
  home,
  library,
  directory,
  queue,
  playlists,
  help
};

/// What the keyboard is currently feeding: normal commands, a text prompt, or
/// the modal playlist picker.
enum class InputMode
{
  Normal,
  Search,         ///< '/' in the directory browser
  SavePlaylist,   ///< 'w' in the queue
  RenamePlaylist, ///< 'R' in the playlists tab
  NewPlaylist,    ///< 'a' in the playlists tab: create an empty one
  NamePlaylistForAdd, ///< picked "new playlist" while adding tracks
  PickPlaylist,   ///< 'A' anywhere: choose the playlist to add to
  Confirm         ///< y/n question
};

/// What a pending Confirm answers.
enum class ConfirmAction
{
  None,
  ClearQueue,
  DeletePlaylist,
  RemovePlaylistEntry
};

/// Which half of the Playlists tab has the cursor.
enum class PlaylistPane
{
  List,   ///< the saved-playlist names
  Entries ///< the tracks of the highlighted playlist
};

/// Scroll + selection state for one list pane. Keeps the cursor inside the
/// viewport without the page-arithmetic the old browser used (which desynced
/// whenever the item count changed under it).
struct ListView
{
  int selected = 0;
  int scroll = 0;

  void reset()
  {
    selected = 0;
    scroll = 0;
  }

  /// Move by delta, wrapping past either end (todo item: top -> bottom).
  void move(int delta, int count, bool wrap = true);
  /// Jump to an absolute index.
  void jump(int index, int count);
  /// Clamp selection/scroll so `selected` is visible in a `height`-row viewport.
  void clamp(int count, int height);
};

/// One row of the library tab. Albums expand in place into their tracks.
struct LibraryRow
{
  int album_index = 0;
  int track_index = -1; ///< -1 for the album header row
};

/// A hit from the recursive '/' search in the directory browser.
struct SearchHit
{
  std::string path;     ///< absolute
  std::string display;  ///< path relative to the search root
  bool is_dir = false;
};

struct UI
{
  // main windows
  WINDOW *header = nullptr;
  WINDOW *main_area = nullptr;
  WINDOW *footer = nullptr;

  // geometry
  int max_rows = 0;
  int max_cols = 0;

  // configuration
  Config::Settings cfg;

  // header clock
  time_t last_header_clock_update = 0;
  Tab last_tab = Tab::home;

  // directory browser
  std::string music_root;
  std::string current_directory;
  std::string last_listed_directory;
  /// Explicit flag instead of comparing paths: the remote root is "", which
  /// equals the initial last_listed_directory and would look already-listed.
  bool listing_valid = false;
  std::vector<Util::DirEntry> items;
  ListView browser;

  // recursive search results (directory tab)
  bool search_results_active = false;
  std::string search_root;
  std::string search_query;
  std::vector<SearchHit> search_hits;
  ListView search_view;

  // library / albums
  std::vector<Library::Album> albums;
  std::vector<bool> album_expanded;
  std::vector<LibraryRow> library_rows;
  ListView library;
  bool library_dirty = true;
  size_t last_album_count = 0;

  // queue
  ListView queue;

  // playlists
  std::vector<std::string> playlist_names;
  std::vector<Playlist::Entry> playlist_preview; ///< entries of `previewed_playlist`, edited in place
  std::string previewed_playlist;
  ListView playlists;
  ListView playlist_entries;
  PlaylistPane playlist_focus = PlaylistPane::List;

  // modal "add to playlist" picker
  std::vector<SongMetadata> pending_add; ///< tracks waiting for a destination
  ListView picker;

  // help screen scroll offset
  ListView help;

  // text prompt / confirmation
  InputMode input_mode = InputMode::Normal;
  ConfirmAction confirm_action = ConfirmAction::None;
  std::string prompt_label;
  std::string input_buffer;

  // transient status line (footer), cleared after a few seconds
  std::string status_message;
  time_t status_expiry = 0;

  // tabs
  Tab current_tab = Tab::home;

  // ascii album art
  Art::ColorMode art_color_mode = Art::ColorMode::ANSI_256;
  Art::RenderMode art_render_mode = Art::RenderMode::BLOCK;
  Art::AsciiCanvas current_art;
  std::string cached_song_path;
  int cached_art_width = 0;
  int cached_art_height = 0;

  // music visualizer / EQ
  Visualizer::State viz_state;

  // where music comes from: local filesystem or a remote orpheusd
  std::unique_ptr<Source::Provider> source;

  // audio player
  MiniAudioPlayer player;
};

class UIManager
{
public:
  UIManager();
  ~UIManager();

  void init(const Config::Settings &cfg, std::unique_ptr<Source::Provider> source, ma_vfs *vfs = nullptr);
  void cleanup();
  void run();

  UI state;

private:
  // window lifecycle
  void createWindows();
  void destroyWindows();
  void handleResize();

  // rendering
  void render();
  void updateHeader();
  void updateFooter();
  void updateMainArea();
  void homeScreen();
  void libraryScreen();
  void directoryScreen();
  void queueScreen();
  void playlistScreen();
  void helpScreen();
  void drawPlaylistPicker();

  // input. Tab handlers return true when they consume the key, so it never
  // leaks into the global bindings (e.g. 'a' means "new playlist" in the
  // Playlists tab but "cycle art palette" everywhere else).
  void handleKey(int ch);
  void handlePromptKey(int ch);
  void handlePickerKey(int ch);
  void handleGlobalKey(int ch);
  bool handleDirectoryKey(int ch);
  bool handleLibraryKey(int ch);
  bool handleQueueKey(int ch);
  bool handlePlaylistKey(int ch);
  bool handleListNavKey(int ch, ListView &view, int count, int height);
  void submitPrompt();

  // actions
  void refreshListing(bool force);
  void enterSelection();
  void enqueueSelection(bool replace_queue);
  void openParentDirectory();
  void runSearch(const std::string &query);
  void rebuildLibraryRows();
  void refreshPlaylists();
  void loadPlaylistPreview();
  void playPlaylist(const std::string &name, bool replace_queue);
  void savePlaylistFromQueue(const std::string &name);
  bool createPlaylist(const std::string &name);
  void beginAddToPlaylist(std::vector<SongMetadata> songs);
  void addPendingToPlaylist(const std::string &name);
  void commitPlaylistEdits();
  std::vector<SongMetadata> selectionForPlaylist();
  void enqueuePath(const std::string &path, bool recursive, std::vector<SongMetadata> &out);
  void startQueued(std::vector<SongMetadata> songs, bool replace_queue);
  void setStatus(const std::string &message);

  // helpers
  int listHeight() const;
  std::string selectedPath() const;
};
