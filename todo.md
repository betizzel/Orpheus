# todo

## done

- [X] different album ascii styled (current is half blocks)
  - uses traditional ascii now
- [X] equalizer that reacts to sound (underneath artist info)
  - FFT analyzer via custom miniaudio tap node + ring buffer
  - styles: block / ansi-art / braille / spectrogram (press `v` to cycle)
- [X] m4a support via ffmpeg
  - custom `ma_decoding_backend_vtable` in `src/player/ffdecoder.cpp`, registered
    on the engine's resource manager; covers aac/alac/opus/ogg/wma/aiff too
- [X] directory sort alphabetically
  - now a natural sort, so `track2` sorts before `track10`
- [X] Faster directory traversal
  - the listing was being re-read from disk every frame: the cache guard
    `dir != last_listed || current_tab == directory` is always true inside the
    directory tab. Now cached on the path alone, with `R` to force a re-read
- [X] ctrl-d to scroll half page down, ctrl-u scroll half page up (vim style)
  - plus ctrl-f/ctrl-b full page and g/G
- [X] / search for a song in subdir
  - recursive, case-insensitive, symlink-safe, capped at 2000 hits
- [X] scrolling to the very top puts you straight to the bottom and vice versa
- [X] remove unsupported files from directory listing
  - `Util::isSupportedAudio` also widens automatically when built with FFmpeg
- [X] update help screen with new keybinds
  - [X] added '-' to go up in directory traversal (i like it from netrw)
- [X] playlist support (create custom playlist or read those playlist files)
  - m3u/m3u8/pls reader, EXTM3U writer, saved playlists tab
  - full in-app authoring: 'a' creates an empty playlist, 'A' files the
    highlighted track/album/folder into one (picker offers "new playlist"),
    'w' snapshots the queue, and the Playlists tab doubles as an editor
    (<Right> to focus tracks, J/K reorder, d removes, saved on every edit)
- [X] lua config
  - music dir, art/visualizer styles, volume, shuffle, repeat, fps, hidden files
- [X] play a whole album / folder at once (`e` to queue, `P` to play now)
- [X] album discovery tab backed by a background tag scanner + on-disk cache
- [X] terminal resize no longer corrupts the layout
- [X] event loop stopped spinning a core (blocking getch on a frame timer)
- [X] ssh remote playback (Herdr-style: daemon where the music is, TUI where you are)
  - `orpheusd` serves listings/tags/art/byte-ranges; never decodes
  - transport is `ssh <host> -- orpheusd --stdio`, so no port, no auth, no TLS
  - client-side miniaudio VFS streams over the session with 2 MiB read-ahead,
    so m4a and flac seek correctly with no second playback path
  - server confines every request to --root, including symlink escapes

## next

- [ ] seek by clicking / dragging the progress bar (ncurses mouse support)
- [ ] gapless playback between queue entries
- [ ] ReplayGain / loudness normalisation from tags
- [ ] MPRIS D-Bus integration (playerctl, media keys)
- [ ] cover art via MusicBrainz when nothing is embedded or on disk
- [ ] filter the library tab by artist/genre
- [ ] configurable keybinds in the Lua config
