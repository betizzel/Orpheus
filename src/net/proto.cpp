// proto.cpp
#include "proto.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace Proto
{

namespace
{

constexpr size_t kBufSize = 64 * 1024;

inline bool needsEscape(unsigned char c)
{
  // Space separates fields, '%' introduces an escape, CR/LF end a line, and
  // control bytes are discarded.
  return c == ' ' || c == '%' || c < 0x21 || c == 0x7F;
}

// hex->int
inline int hexValue(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

} // namespace

std::string encodeField(std::string_view raw)
{
  static const char *kHex = "0123456789ABCDEF";

  std::string out;
  out.reserve(raw.size());
  for (char ch : raw)
  {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (needsEscape(c))
    {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
    else
    {
      out.push_back(ch);
    }
  }

  // An empty field would vanish into the space separator if not treated correctly.
  if (out.empty())
    out = "%00";
  return out;
}

std::string decodeField(std::string_view wire)
{
  // empty field
  if (wire == "%00")
    return {};

  std::string out;
  out.reserve(wire.size());
  for (size_t i = 0; i < wire.size(); ++i)
  {
    if (wire[i] != '%' || i + 2 >= wire.size())
    {
      out.push_back(wire[i]);
      continue;
    }

    const int hi = hexValue(wire[i + 1]);
    const int lo = hexValue(wire[i + 2]);
    if (hi < 0 || lo < 0)
    {
      // Malformed escape: keep the byte rather than failing. A hostile peer
      // gets a mangled string, never a thrown exception through the loop.
      out.push_back(wire[i]);
      continue;
    }

    out.push_back(static_cast<char>((hi << 4) | lo));
    i += 2;
  }
  return out;
}

std::vector<std::string_view> splitFields(std::string_view line, size_t max_fields)
{
  std::vector<std::string_view> fields;
  size_t i = 0;
  while (i < line.size() && fields.size() < max_fields)
  {
    while (i < line.size() && line[i] == ' ')
      ++i;
    if (i >= line.size())
      break;
    const size_t start = i;
    while (i < line.size() && line[i] != ' ')
      ++i;
    fields.push_back(line.substr(start, i - start));
  }
  return fields;
}

bool sanitizeWirePath(std::string_view relative, std::string &out)
{
  out.clear();
  if (relative.find('\0') != std::string_view::npos)
    return false;
  if (!relative.empty() && relative.front() == '/')
    return false;

  std::vector<std::string_view> parts;
  size_t i = 0;
  while (i < relative.size())
  {
    while (i < relative.size() && relative[i] == '/')
      ++i;

    const size_t start = i;

    while (i < relative.size() && relative[i] != '/')
      ++i;

    if (i == start)
      break;

    const std::string_view part = relative.substr(start, i - start);
    if (part == ".")
      continue;

    if (part == "..")
    {
      // Refuse rather than popping: a client has no business walking up, and
      // silently clamping would make "../../etc" resolve to the root.
      return false;
    }
    parts.push_back(part);
  }

  for (size_t p = 0; p < parts.size(); ++p)
  {
    if (p)
      out.push_back('/');
    out.append(parts[p]);
  }
  return true;
}

FdStream::FdStream(int read_fd, int write_fd, bool owns)
    : read_fd_(read_fd), write_fd_(write_fd), owns_(owns), buf_(kBufSize)
{
}

FdStream::~FdStream()
{
  close();
}

void FdStream::close()
{
  if (owns_)
  {
    if (read_fd_ >= 0)
      ::close(read_fd_);
    if (write_fd_ >= 0 && write_fd_ != read_fd_)
      ::close(write_fd_);
  }
  read_fd_ = -1;
  write_fd_ = -1;
  good_ = false;
}

void FdStream::setInterrupt(const volatile std::sig_atomic_t *flag)
{
  interrupt_ = flag;
}

bool FdStream::good() const
{
  return good_;
}

bool FdStream::fill()
{
  if (head_ < tail_)
    return true;
  if (read_fd_ < 0)
    return false;

  head_ = 0;
  tail_ = 0;

  for (;;)
  {
    const ssize_t n = ::read(read_fd_, buf_.data(), buf_.size());
    if (n > 0)
    {
      tail_ = static_cast<size_t>(n);
      return true;
    }
    if (n == 0)
    {
      good_ = false; // clean EOF: peer hung up
      return false;
    }
    if (errno == EINTR && !(interrupt_ && *interrupt_))
      continue; // a signal is not an error unless it asked us to stop
    good_ = false;
    return false;
  }
}

bool FdStream::readExact(void *dst, size_t n)
{
  auto *out = static_cast<unsigned char *>(dst);
  size_t done = 0;
  while (done < n)
  {
    if (head_ >= tail_ && !fill())
      return false;
    const size_t take = std::min(n - done, tail_ - head_);
    std::memcpy(out + done, buf_.data() + head_, take);
    head_ += take;
    done += take;
  }
  return true;
}

bool FdStream::readLine(std::string &out)
{
  out.clear();
  for (;;)
  {
    if (head_ >= tail_ && !fill())
      return false;

    for (size_t i = head_; i < tail_; ++i)
    {
      if (buf_[i] != '\n')
        continue;
      out.append(buf_.data() + head_, i - head_);
      head_ = i + 1;
      return true;
    }

    out.append(buf_.data() + head_, tail_ - head_);
    head_ = tail_;
    if (out.size() > kMaxLine)
    {
      // Overlong line: the peer is broken or hostile. Fail the connection
      // instead of growing the buffer without bound.
      good_ = false;
      return false;
    }
  }
}

bool FdStream::writeAll(const void *src, size_t n)
{
  if (write_fd_ < 0)
    return false;

  const auto *in = static_cast<const unsigned char *>(src);
  size_t done = 0;
  while (done < n)
  {
    // send(MSG_NOSIGNAL) turns a vanished peer into EPIPE instead of a
    // SIGPIPE that would kill the process (and, in the client, strand the
    // terminal in ncurses mode). Pipes fall back to write(), where the 
    // daemon relies on its process-wide SIG_IGN.
    ssize_t w = 0;
    if (use_send_)
    {
      w = ::send(write_fd_, in + done, n - done, MSG_NOSIGNAL);
      if (w < 0 && errno == ENOTSOCK)
      {
        use_send_ = false;
        continue;
      }
    }
    else
    {
      w = ::write(write_fd_, in + done, n - done);
    }
    if (w > 0)
    {
      done += static_cast<size_t>(w);
      continue;
    }
    if (w < 0 && errno == EINTR && !(interrupt_ && *interrupt_))
      continue;
    good_ = false;
    return false;
  }
  return true;
}

} // namespace Proto
