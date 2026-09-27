// colorpair.hpp: shared ncurses color-pair cache
//
// Both the album-art renderer (ui.cpp) and the visualizer (visualizer.cpp)
// map an (fg, bg) ANSI-256 color pair to a ncurses COLOR_PAIR id. ncurses
// keeps ONE global pair table keyed by id, so the two must share a single
// id allocator. Two independent counters eventually hand out the same id
// for different (fg, bg) combos; the second init_pair() silently redefines
// the pair and cells using it (e.g. album-art half-blocks) flip to the wrong
// colors, visible as random "specks" on the art, especially in BLOCK mode
// which generates many unique (fg, bg) pairs.
//
// This header provides one shared cache so an id is never handed out twice.
// `inline` + a function-local `static` collapses all copies across
// translation units into a single instance (Meyers-singleton idiom).
#pragma once
#include <map>
#include <ncurses.h>
#include <utility>

inline int getSharedColorPair(int fg, int bg)
{
  static std::map<std::pair<int, int>, int> cache;
  static int next_id = 1;

  auto key = std::make_pair(fg, bg);
  auto it = cache.find(key);

  if (it != cache.end())
  {
    return it->second;
  }

  int id = next_id++;
  init_pair(id, fg, bg);
  cache[key] = id;

  return id;
}
