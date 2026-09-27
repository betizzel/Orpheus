// visualizer.cpp: FFT analyzer + renderers
#include "visualizer.hpp"
#include "art.hpp"
#include "colorpair.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ncurses.h>

namespace Visualizer
{

// Color helpers: small ANSI-256 palette + cached ncurses color pairs

// note this is a fallback
// GRAYSCALE: black -> dark gray -> gray -> light gray -> white
static const int PALETTE[16] = {
    16,
    232,
    233,
    234, // black -> very dark gray
    235,
    236,
    237,
    238, // dark gray
    239,
    240,
    241,
    242, // gray
    244,
    248,
    252,
    255 // light gray -> white
};

static int paletteColor(float v, const int ramp[16])
{
  if (v <= 0.0f)
  {
    return ramp[0];
  }

  if (v >= 1.0f)
  {
    return ramp[15];
  }

  return ramp[static_cast<int>(v * 16)];
}

// Map a normalized level (0..1) to an ASCII character using the same ramp
// as the album-art DETAILED renderer so the visualizer matches the project's
// ASCII-art style.
static wchar_t asciiCharFromLevel(float level)
{
  float clamped = std::clamp(level, 0.0f, 1.0f);
  uint32_t gray = static_cast<uint32_t>(clamped * 255.0f);
  uint32_t rgb = (gray << 16) | (gray << 8) | gray;

  return static_cast<wchar_t>(Art::MapRGBToChar(rgb));
}

// FFT: iterative radix-2 Cooley-Tukey. n must be a power of 2.
static void fftRadix2(std::complex<float> *a, int n)
{
  // bit-reversal permutation
  for (int i = 1, j = 0; i < n; ++i)
  {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1)
    {
      j ^= bit;
    }

    j ^= bit;

    if (i < j)
    {
      std::swap(a[i], a[j]);
    }
  }

  // butterfly
  for (int len = 2; len <= n; len <<= 1)
  {
    float ang = -2.0f * 3.14159265358979323846f / static_cast<float>(len);
    std::complex<float> wlen(std::cos(ang), std::sin(ang));

    for (int i = 0; i < n; i += len)
    {
      std::complex<float> w(1.0f, 0.0f);

      for (int k = 0; k < len / 2; ++k)
      {
        std::complex<float> u = a[i + k];
        std::complex<float> v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wlen;
      }
    }
  }
}

Analyzer::Analyzer()
{
  hann.resize(FFT_N);

  for (int i = 0; i < FFT_N; ++i)
  {
    // Hann window: w[n] = 0.5 - 0.5*cos(2*pi*n/(N-1))
    hann[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979f * i / (FFT_N - 1));
  }

  buf.resize(FFT_N);
  magnitudes.resize(FFT_N / 2);
}

void Analyzer::configure(int bars, uint32_t sample_rate)
{
  if (bars == bar_count && sample_rate != 0)
  {
    return;
  }

  bar_count = bars;
  bin_lo.assign(bars, 0);
  bin_hi.assign(bars, 0);
  levels.assign(bars, 0.0f);
  peaks.assign(bars, 0.0f);

  if (bars <= 0 || sample_rate == 0)
  {
    return;
  }

  // Log-spaced frequency bins.
  // Min ~30 Hz (below that is mostly rumble), max ~ Nyquist or 16 kHz.
  const float f_min = 30.0f;
  const float f_max = std::min(static_cast<float>(sample_rate) * 0.5f, 16000.0f);
  const float log_min = std::log(f_min);
  const float log_max = std::log(f_max);

  for (int i = 0; i < bars; ++i)
  {
    float f_lo = std::exp(log_min + (log_max - log_min) * (i) / bars);
    float f_hi = std::exp(log_min + (log_max - log_min) * (i + 1) / bars);
    int lo = static_cast<int>(f_lo * FFT_N / static_cast<float>(sample_rate));
    int hi = static_cast<int>(f_hi * FFT_N / static_cast<float>(sample_rate));

    if (hi <= lo)
    {
      hi = lo + 1;
    }

    lo = std::max(1, lo);
    hi = std::min(FFT_N / 2, hi);
    bin_lo[i] = lo;
    bin_hi[i] = hi;
  }
}

bool Analyzer::update(RingBuffer &ring, uint32_t /*sample_rate*/)
{
  if (bar_count <= 0)
  {
    return false;
  }

  if (ring.available() < FFT_N)
  {
    return false;
  }

  // Pull FFT_N samples into the complex buffer (real input, imaginary = 0).
  // We read the OLDEST FFT_N samples currently in the ring.
  float tmp[FFT_N];
  size_t got = ring.read(tmp, FFT_N);

  if (got < static_cast<size_t>(FFT_N))
  {
    return false; // shouldn't happen, but be safe
  }

  for (int i = 0; i < FFT_N; ++i)
  {
    buf[i] = std::complex<float>(tmp[i] * hann[i], 0.0f);
  }

  fftRadix2(buf.data(), FFT_N);

  // Magnitudes for the first half (second half is the mirror image)
  for (int k = 0; k < FFT_N / 2; ++k)
  {
    float re = buf[k].real();
    float im = buf[k].imag();
    float mag = std::sqrt(re * re + im * im) * 2.0f / FFT_N;
    magnitudes[k] = mag;
  }

  // Aggregate bins into bars. Use max for punchy look, then convert to
  // decibels (log) so the full dynamic range of music is visible.
  // Full-scale sine = 0 dB; quiet content sits around -60 dB. We map
  // -60 dB..0 dB -> 0..1.
  for (int i = 0; i < bar_count; ++i)
  {
    float mx = 0.0f;

    for (int k = bin_lo[i]; k < bin_hi[i]; ++k)
    {
      mx = std::max(mx, magnitudes[k]);
    }

    // Log scaling. Add tiny epsilon so log10(0) doesn't explode.
    float db = 20.0f * std::log10(mx + 1e-9f);
    float target = (db + 60.0f) / 60.0f; // -60 dB -> 0, 0 dB -> 1

    if (target < 0.0f)
    {
      target = 0.0f;
    }

    if (target > 1.0f)
    {
      target = 1.0f;
    }

    // Noise gate: if the linear magnitude is tiny, force to zero.
    if (mx < noise_floor)
    {
      target = 0.0f;
    }

    // Time smoothing
    levels[i] = levels[i] * (1.0f - smooth) + target * smooth;

    // Peak hold with decay
    if (levels[i] > peaks[i])
    {
      peaks[i] = levels[i];
    }
    else
    {
      peaks[i] = std::max(0.0f, peaks[i] - peak_decay);
    }
  }

  return true;
}

// State
void State::resetSpectrogram()
{
  spec_grid.assign(spec_cols * spec_rows, 0.0f);
  spec_head = 0;
}

// Renderers: one per style

// Block bars: ASCII-art bars drawn bottom-up.
// The body uses a character chosen from the level, and the partial cap uses
// the same ASCII ramp so the whole thing matches the album-art style.
static void renderBlockBars(
    WINDOW *win, int y, int x, int w, int h, const int ramp[16], const Analyzer &a)
{
  static const wchar_t peak_glyph = L'-';

  int bars = std::min(a.bar_count, w);

  for (int i = 0; i < bars; ++i)
  {
    float level = a.levels[i];
    float peak = a.peaks[i];
    int col = x + i;

    float filled = level * h;
    int full_rows = static_cast<int>(filled);
    float remainder = filled - full_rows;

    int fg = paletteColor(level, ramp);
    int pair_id = getSharedColorPair(fg, 0);

    wchar_t body_ch = asciiCharFromLevel(level);

    for (int r = 0; r < h; ++r)
    {
      int row = y + h - 1 - r; // row 0 = bottom
      wchar_t ch = L' ';

      if (r < full_rows)
      {
        ch = body_ch;
      }
      else if (r == full_rows && remainder > 0.0f)
      {
        ch = asciiCharFromLevel(remainder);
      }

      if (ch != L' ')
      {
        cchar_t cc;
        setcchar(&cc, &ch, 0, pair_id, nullptr);
        mvwadd_wch(win, row, col, &cc);
      }
    }

    // Peak indicator: one row above the bar top
    int peak_row_float = static_cast<int>(peak * h);
    int peak_row = y + h - 1 - peak_row_float;

    if (peak_row >= y && peak_row < y + h && peak_row_float > full_rows)
    {
      int peak_pair = getSharedColorPair(paletteColor(peak * 0.7f + 0.3f, ramp), 0);
      cchar_t cc;
      setcchar(&cc, &peak_glyph, 0, peak_pair, nullptr);
      mvwadd_wch(win, peak_row, col, &cc);
    }
  }
}

// ANSI-art bars: half-block bars in the style of the BLOCK album-art
// renderer. Each terminal cell uses ▀ with fg = top sub-row color and
// bg = bottom sub-row color, giving 2× vertical resolution. Empty sub-rows
// fall back to the default background (no dim fill) for a clean look.
static void renderAnsiArtBars(
    WINDOW *win, int y, int x, int w, int h, const int ramp[16], const Analyzer &a)
{
  int bars = std::min(a.bar_count, w);

  for (int i = 0; i < bars; ++i)
  {
    float level = a.levels[i];
    int col = x + i;
    int color = paletteColor(level, ramp);

    // 2 sub-rows per cell: total lit sub-rows = level * 2 * h
    float total = level * 2.0f * h;

    for (int r = 0; r < h; ++r)
    {
      int row = y + h - 1 - r; // r=0 is bottom cell
      float in_this = total - r * 2.0f;
      int lit = static_cast<int>(std::ceil(in_this));

      if (lit < 0)
      {
        lit = 0;
      }

      if (lit > 2)
      {
        lit = 2;
      }

      if (lit == 0)
      {
        continue; // empty: leave default background
      }

      // lit == 1: top half only (▀ with fg=color, bg=default)
      // lit == 2: both halves  (▀ with fg=color, bg=color) -> solid block
      int bg = (lit == 2) ? color : 0;
      int pair_id = getSharedColorPair(color, bg);

      cchar_t cc;
      wchar_t ch = L'▀';
      setcchar(&cc, &ch, 0, pair_id, nullptr);
      mvwadd_wch(win, row, col, &cc);
    }
  }
}

// Braille bars: 4 sub-rows per cell using the left column of dots
// (dots 1, 2, 3, 7 -> bits 0x01, 0x02, 0x04, 0x40).
static void renderBrailleBars(
    WINDOW *win, int y, int x, int w, int h, const int ramp[16], const Analyzer &a)
{
  int bars = std::min(a.bar_count, w);

  // For a single-column braille cell, the 4 left dots in fill order
  // (bottom -> top): dot 7 (0x40), dot 3 (0x04), dot 2 (0x02), dot 1 (0x01).
  static const int fill_bits[4] = {0x40, 0x04, 0x02, 0x01};

  for (int i = 0; i < bars; ++i)
  {
    float level = a.levels[i];
    int col = x + i;
    int fg = paletteColor(level, ramp);
    int pair_id = getSharedColorPair(fg, 0);

    // Total sub-pixel fill = level * 4 * h
    float total = level * 4.0f * h;

    for (int r = 0; r < h; ++r)
    {
      int row = y + h - 1 - r; // r=0 is bottom cell

      // How many dots lit in this cell? Clamp 0..4.
      float in_this = total - r * 4.0f;
      int lit = static_cast<int>(std::ceil(in_this));
      if (lit < 0)
      {
        lit = 0;
      }

      if (lit > 4)
      {
        lit = 4;
      }

      if (lit == 0)
      {
        continue;
      }

      int bits = 0;
      for (int d = 0; d < lit; ++d)
      {
        bits |= fill_bits[d];
      }

      wchar_t ch = static_cast<wchar_t>(0x2800 + bits);
      cchar_t cc;
      setcchar(&cc, &ch, 0, pair_id, nullptr);
      mvwadd_wch(win, row, col, &cc);
    }
  }
}

// Spectrogram: scrolling heatmap of frequency (rows) over time (cols).
// Uses a circular column buffer to avoid memmove each frame.
static void renderSpectrogram(WINDOW *win,
                              int y,
                              int x,
                              int w,
                              int h,
                              const int ramp[16],
                              State &state,
                              uint32_t /*sample_rate*/)
{
  // (Re)allocate grid if dimensions changed
  if (state.spec_cols != w || state.spec_rows != h)
  {
    state.spec_cols = w;
    state.spec_rows = h;
    state.resetSpectrogram();
  }

  // Write a new column into the grid at spec_head, then advance.
  // Each row r (0=bottom=low freq) aggregates the FFT magnitudes in that row's bin range.
  // Reuse the analyzer's bin_lo/bin_hi (it was configured with bar_count = h).
  if (h > 0 && w > 0 && static_cast<int>(state.analyzer.magnitudes.size()) > 0)
  {
    for (int r = 0; r < h; ++r)
    {
      int i = h - 1 - r; // top row = highest frequency bin
      if (i >= state.analyzer.bar_count)
      {
        continue;
      }

      float mx = 0.0f;
      for (int k = state.analyzer.bin_lo[i]; k < state.analyzer.bin_hi[i]; ++k)
      {
        mx = std::max(mx, state.analyzer.magnitudes[k]);
      }

      float v = std::sqrt(mx) * 2.5f;

      if (v < state.analyzer.noise_floor)
      {
        v = 0.0f;
      }

      if (v > 1.0f)
      {
        v = 1.0f;
      }

      state.spec_grid[r * state.spec_cols + state.spec_head] = v;
    }

    state.spec_head = (state.spec_head + 1) % state.spec_cols;
  }

  // Render: screen col c -> grid col (spec_head + c) % spec_cols
  // spec_head is the oldest (leftmost), and we just advanced it, so the
  // newest column is at (spec_head - 1 + spec_cols) % spec_cols = rightmost.
  for (int c = 0; c < w; ++c)
  {
    int grid_col = (state.spec_head + c) % state.spec_cols;

    for (int r = 0; r < h; ++r)
    {
      float v = state.spec_grid[r * state.spec_cols + grid_col];
      int color = paletteColor(v, ramp);
      int pair_id = getSharedColorPair(color, 0);

      wchar_t ch = asciiCharFromLevel(v);
      cchar_t cc;
      setcchar(&cc, &ch, 0, pair_id, nullptr);
      mvwadd_wch(win, y + h - 1 - r, x + c, &cc);
    }
  }
}

// Top-level render: picks the active style + throttles FFT to ~30 Hz
static long nowMs()
{
  auto t = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(t).count();
}

void render(WINDOW *win,
            int y,
            int x,
            int width,
            int height,
            State &state,
            RingBuffer &ring,
            uint32_t sample_rate)
{
  if (width <= 0 || height <= 0 || sample_rate == 0)
  {
    return;
  }

  // Pick the color ramp: the album-art palette when available and enabled,
  // otherwise the fixed PALETTE above. The ramp pointer is dereferenced in
  // hot per-frame paths, so resolve it once here.
  const int *ramp =
      (state.use_dynamic_palette && state.has_dynamic_palette) ? state.dynamic_palette : PALETTE;

  // Throttle FFT + analysis to ~30 Hz. Rendering runs every call so bars
  // keep decaying smoothly. Peaks/levels update only on FFT tick.
  long t = nowMs();
  if (t - state.last_fft_ms >= 33)
  {
    state.last_fft_ms = t;

    // Reconfigure binning when style or dimensions change.
    // Bars styles: bar_count = width. Spectrogram: bar_count = height (rows).
    int desired_bars = (state.style == Style::SPECTROGRAM) ? height : width;

    if (state.analyzer.bar_count != desired_bars || state.last_sample_rate != sample_rate)
    {
      state.analyzer.configure(desired_bars, sample_rate);
      state.last_sample_rate = sample_rate;

      if (state.style == Style::SPECTROGRAM)
      {
        state.resetSpectrogram();
      }
    }

    state.analyzer.update(ring, sample_rate);
  }

  switch (state.style)
  {
  case Style::BLOCK:
    renderBlockBars(win, y, x, width, height, ramp, state.analyzer);
    break;

  case Style::ANSI_ART:
    renderAnsiArtBars(win, y, x, width, height, ramp, state.analyzer);
    break;

  case Style::BRAILLE:
    renderBrailleBars(win, y, x, width, height, ramp, state.analyzer);
    break;

  case Style::SPECTROGRAM:
    renderSpectrogram(win, y, x, width, height, ramp, state, sample_rate);
    break;
  }
}

} // namespace Visualizer
