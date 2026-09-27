#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <vector>

class RingBuffer
{
public:
  // capacity is required to be a power of 2
  explicit RingBuffer(size_t capacity) : buf(capacity), mask(capacity - 1), head(0), tail(0)
  {
    assert((capacity & (capacity - 1)) == 0 && "RingBuffer capacity must be a power of 2");
  }

  // return written frames
  size_t write(const float *src, size_t n)
  {
    size_t h = head.load(std::memory_order_relaxed);
    size_t t = tail.load(std::memory_order_acquire);

    // wrap around powers of 2
    size_t free = mask + 1 - (h - t);
    size_t count = std::min(n, free);

    for (size_t i = 0; i < count; ++i)
    {
      buf[(h + i) & mask] = src[i];
    }

    head.store(h + count, std::memory_order_release);
    return count;
  }

  // return frames read from UI thread
  size_t read(float *dst, size_t n)
  {
    size_t h = head.load(std::memory_order_acquire);
    size_t t = tail.load(std::memory_order_relaxed);
    size_t avail = h - t;
    size_t count = std::min(n, avail);

    for (size_t i = 0; i < count; ++i)
    {
      dst[i] = buf[(t + i) & mask];
    }

    tail.store(t + count, std::memory_order_release);
    return count;
  }

  size_t available() const
  {
    return head.load(std::memory_order_acquire) - tail.load(std::memory_order_acquire);
  }

  void clear()
  {
    head.store(0, std::memory_order_release);
    tail.store(0, std::memory_order_release);
  }

private:
  std::vector<float> buf;
  size_t mask;
  std::atomic<size_t> head, tail;
};
