-- Orpheus configuration sample.
--
-- Copy to ~/.config/orpheus/orpheus.lua (Orpheus writes this file for you on
-- first run if it does not exist). Every key is optional; anything you leave
-- out keeps its default. Invalid values log a warning and fall back.
--
-- Both styles work: return a table (preferred), or assign plain globals.

return {
  -- Where the Directory tab starts and what the library scanner indexes.
  music_dir = "~/Music",

  -- Album art colouring: "ansi" (256-colour) or "grayscale".
  art_color_mode = "ansi",
  -- Album art glyphs: "block" (half blocks) or "detailed" (ASCII ramp).
  art_render_mode = "block",

  -- Visualizer: "block", "ansi", "braille" or "spectrogram".
  visualizer = "block",
  -- Colour the EQ with the palette extracted from the current album art.
  dynamic_palette = true,

  -- Show dot-files in the directory browser.
  show_hidden = false,

  -- UI redraw cap (5-120). Lower it on a slow terminal or over SSH.
  fps = 30,

  -- Startup playback state.
  volume = 1.0,     -- 0.0 .. 1.0
  shuffle = false,
  repeat_mode = "off", -- "off", "all" or "one"

  -- Index the library in the background at launch so the Library tab is
  -- populated. Turn off for very large collections on slow storage.
  scan_on_start = true,

  -- Remote playback: set to an ssh host to attach to an orpheusd there at
  -- startup (same as `orpheus --remote <host>`). Audio still plays locally.
  -- remote_host = "nas",
  -- remote_command = "orpheusd --stdio",
}
