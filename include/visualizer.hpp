// visualizer.hpp: FFT-based music visualizer
#pragma once
#include "ringbuffer.hpp"
#include <complex>
#include <cstdint>
#include <ncurses.h>
#include <string>
#include <vector>

namespace Visualizer
{

enum class Style
{
  BLOCK,       // classic ▁▂▃▄▅▆▇█ bars
  ANSI_ART,    // colored bars with ASCII-art caps
  BRAILLE,     // braille dots (4 sub-rows per cell)
  SPECTROGRAM, // scrolling frequency-over-time heatmap
};

inline std::string styleName(Style s)
{
  switch (s)
  {
  case Style::BLOCK:
    return "block";
  case Style::ANSI_ART:
    return "ansi-art";
  case Style::BRAILLE:
    return "braille";
  case Style::SPECTROGRAM:
    return "spectrogram";
  }

  return "?";
}

// Runs the FFT + log binning + smoothing. Stateful (holds smoothed bar
// levels and per-bar peaks). One instance per visualizer.
struct Analyzer
{
  static constexpr int FFT_N = 1024; // power of 2; gives 512 useful bins

  // FFT workspace
  std::vector<float> hann;              // precomputed Hann window, size FFT_N
  std::vector<std::complex<float>> buf; // FFT working buffer, size FFT_N
  std::vector<float> magnitudes;        // per-bin magnitudes, size FFT_N/2

  // Bar state (recomputed when bar_count changes)
  int bar_count = 0;
  std::vector<int> bin_lo;   // inclusive lower bin index per bar
  std::vector<int> bin_hi;   // exclusive upper bin index per bar
  std::vector<float> levels; // smoothed bar levels, 0..1
  std::vector<float> peaks;  // peak-hold with slow decay, 0..1

  // Tuning
  float smooth = 0.30f;      // higher = snappier, lower = lazier
  float peak_decay = 0.02f;  // peak fall per frame
  float noise_floor = 0.02f; // magnitudes below this -> 0

  Analyzer();

  // Recompute log binning for a new bar count + sample rate
  void configure(int bars, uint32_t sample_rate);

  // Pull FFT_N samples from the ring buffer, run FFT, update levels/peaks.
  // Returns false if there weren't enough samples yet.
  bool update(RingBuffer &ring, uint32_t sample_rate);
};

// Persistent state across frames (mostly for the spectrogram's scrolling
// grid). Lives in UI struct.
struct State
{
  Style style = Style::BLOCK;
  Analyzer analyzer;

  // Color palette: either the fixed PALETTE in visualizer.cpp, or a
  // 16-step ramp extracted from the current album art (see art.cpp's
  // extractPalette). When use_dynamic_palette && has_dynamic_palette,
  // the visualizer renders in the album's colors; otherwise it falls
  // back to the fixed ramp. Toggled at runtime with the 'C' key.
  int dynamic_palette[16] = {};
  bool has_dynamic_palette = false;
  bool use_dynamic_palette = true;

  // Spectrogram scrolling buffer: rows × cols of normalized magnitudes (0..1).
  // Uses a circular column head to avoid memmove each frame.
  std::vector<float> spec_grid;
  int spec_cols = 0;
  int spec_rows = 0;
  int spec_head = 0; // next column to write
  uint32_t last_sample_rate = 0;

  // Throttle: only run FFT/analyze every N ms to save CPU
  long last_fft_ms = 0;

  void resetSpectrogram();
};

// Render the active style into the given window region.
// (x, y) is the top-left cell, width/height are the cell dimensions.
// Call every frame from the UI thread.
void render(WINDOW *win,
            int y,
            int x,
            int width,
            int height,
            State &state,
            RingBuffer &ring,
            uint32_t sample_rate);

} // namespace Visualizer
