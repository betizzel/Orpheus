// remote.cpp
// client session over SSH plus the streaming miniaudio VFS.
#include "remote.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Remote
{

namespace
{

bool parseU64(std::string_view text, uint64_t &out)
{
  const auto *begin = text.data();
  const auto *end = begin + text.size();
  const auto result = std::from_chars(begin, end, out);

  return result.ec == std::errc() && result.ptr == end;
}

int toInt(std::string_view text)
{
  long long value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);

  return result.ec == std::errc() ? static_cast<int>(value) : 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

Session::~Session()
{
  if (stream_ && stream_->good())
  {
    // Best-effort polite shutdown so the server can drop its scanner.
    const std::string bye = "BYE\n";
    stream_->writeAll(bye.data(), bye.size());
  }

  stream_.reset();

  if (child_ > 0)
  {
    // Closing the pipe makes ssh exit sp we need to make sure
    // we clean it up so it's not a zombie process.
    int status = 0;
    ::waitpid(child_, &status, 0);
    child_ = -1;
  }
}

bool Session::alive() const
{
  std::lock_guard<std::mutex> guard(lock_);
  return stream_ && stream_->good();
}

uint64_t Session::requestCount() const
{
  std::lock_guard<std::mutex> guard(lock_);
  return requests_;
}

bool Session::exchange(const std::string &request, std::string &response)
{
  if (!stream_ || !stream_->good())
  {
    return false;
  }

  std::string line = request;
  line.push_back('\n');

  if (!stream_->writeAll(line.data(), line.size()))
  {
    return false;
  }

  ++requests_;
  return stream_->readLine(response);
}

bool Session::readPayload(const std::string &response,
                          std::vector<unsigned char> &out,
                          bool &had_payload)
{
  had_payload = false;
  out.clear();

  const auto fields = Proto::splitFields(response, 2);
  if (fields.empty() || fields[0] != "DATA")
  {
    return true; // not a payload response; caller inspects `response`
  }

  uint64_t count = 0;
  if (fields.size() < 2 || !parseU64(fields[1], count) || count > Proto::kMaxChunk)
  {
    return abandon();
  }

  had_payload = true;
  out.resize(static_cast<size_t>(count));

  return count == 0 || stream_->readExact(out.data(), out.size());
}

bool Session::abandon()
{
  stream_->close();
  return false;
}

bool Session::handshake(std::string &error)
{
  std::lock_guard<std::mutex> guard(lock_);

  std::string response;
  if (!exchange("HELLO " + std::to_string(Proto::kVersion), response))
  {
    error = "no response from orpheusd (is it installed on the remote host?)";
    return false;
  }

  const auto fields = Proto::splitFields(response, 3);
  if (fields.empty() || fields[0] != "OK")
  {
    error = fields.size() > 1 ? Proto::decodeField(fields[1]) : "handshake refused";
    return false;
  }

  if (fields.size() < 2 || toInt(fields[1]) != Proto::kVersion)
  {
    error = "protocol version mismatch (client speaks " + std::to_string(Proto::kVersion) + ")";
    return false;
  }

  label_ = fields.size() > 2 ? Proto::decodeField(fields[2]) : "remote";
  return true;
}

std::unique_ptr<Session> Session::adopt(std::unique_ptr<Proto::Stream> stream,
                                        const std::string &description,
                                        std::string &error)
{
  std::unique_ptr<Session> session(new Session());
  session->stream_ = std::move(stream);
  session->description_ = description;

  if (!session->handshake(error))
  {
    return nullptr;
  }

  return session;
}

std::unique_ptr<Session> Session::connectSsh(const std::string &host,
                                             const std::string &remote_command,
                                             const std::vector<std::string> &extra_args,
                                             std::string &error)
{
  // argv is assembled BEFORE fork():
  // between fork() and exec() in a process that has other threads
  // (the library scanner does), only async-signal-safe calls are legal.
  // Allocating there can deadlock on the malloc lock.

  std::vector<const char *> argv;
  argv.reserve(extra_args.size() + 5);
  argv.push_back("ssh");

  for (const auto &arg : extra_args)
  {
    argv.push_back(arg.c_str());
  }

  argv.push_back(host.c_str());
  argv.push_back("--"); // stops ssh parsing the remote command as options
  argv.push_back(remote_command.c_str());
  argv.push_back(nullptr);

  int sockets[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
  {
    error = "socketpair() failed";
    return nullptr;
  }

  const pid_t pid = ::fork();

  // error case
  if (pid < 0)
  {
    ::close(sockets[0]);
    ::close(sockets[1]);
    error = "fork() failed";

    return nullptr;
  }

  // Child: wire the socket to stdin/stdout and become ssh.
  // stderr is left alone on purpose so SSH's own prompts and errors reach the user.
  if (pid == 0)
  {
    ::close(sockets[0]);
    ::dup2(sockets[1], STDIN_FILENO);
    ::dup2(sockets[1], STDOUT_FILENO);
    ::close(sockets[1]);
    ::execvp("ssh", const_cast<char *const *>(argv.data()));
    ::_exit(127); // execvp only returns on failure
  }

  ::close(sockets[1]);

  std::unique_ptr<Session> session(new Session());
  session->stream_ = std::make_unique<Proto::FdStream>(sockets[0], sockets[0], true);
  session->description_ = "ssh://" + host;
  session->child_ = pid;

  if (!session->handshake(error))
  {
    // Distinguish "ssh died" from "server misbehaved"
    // WIFEXITED :(
    int status = 0;

    if (::waitpid(pid, &status, WNOHANG) == pid && WIFEXITED(status) && WEXITSTATUS(status) != 0)
    {
      error =
          "ssh to " + host + " failed (exit " + std::to_string(WEXITSTATUS(status)) + "): " + error;
    }

    session->child_ = -1;
    return nullptr;
  }

  return session;
}

std::unique_ptr<Session> Session::connectUnix(const std::filesystem::path &socket_path,
                                              std::string &error)
{
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
  {
    error = "socket() failed";
    return nullptr;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const std::string path = socket_path.string();

  if (path.size() >= sizeof(addr.sun_path))
  {
    ::close(fd);
    error = "socket path too long";

    return nullptr;
  }

  std::memcpy(addr.sun_path, path.c_str(), path.size());

  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
  {
    ::close(fd);
    error = "cannot connect to " + path;

    return nullptr;
  }

  std::unique_ptr<Session> session(new Session());
  session->stream_ = std::make_unique<Proto::FdStream>(fd, fd, true);
  session->description_ = "unix://" + path;

  if (!session->handshake(error))
  {
    return nullptr;
  }

  return session;
}

bool Session::list(const std::string &path, std::vector<Util::DirEntry> &out)
{
  std::lock_guard<std::mutex> guard(lock_);
  out.clear();

  std::string response;
  if (!exchange("LIST " + Proto::encodeField(path), response))
  {
    return false;
  }

  const auto head = Proto::splitFields(response, 2);
  if (head.empty() || head[0] != "OK" || head.size() < 2)
  {
    return false;
  }

  uint64_t count = 0;
  if (!parseU64(head[1], count))
  {
    return abandon();
  }

  out.reserve(static_cast<size_t>(count));

  for (uint64_t i = 0; i < count; ++i)
  {
    std::string row;
    if (!stream_->readLine(row))
    {
      return false;
    }

    const auto fields = Proto::splitFields(row, 4);
    if (fields.empty())
    {
      return abandon();
    }

    if (fields[0] == "D" && fields.size() >= 2)
    {
      out.push_back({Proto::decodeField(fields[1]), true});
    }
    else if (fields[0] == "F" && fields.size() >= 4)
    {
      out.push_back({Proto::decodeField(fields[3]), false});
    }
    else
    {
      return abandon();
    }
  }

  return true;
}

bool Session::tags(const std::string &path, SongMetadata &out)
{
  std::lock_guard<std::mutex> guard(lock_);

  std::string response;
  if (!exchange("TAGS " + Proto::encodeField(path), response))
  {
    return false;
  }

  const auto fields = Proto::splitFields(response, 6);
  if (fields.size() < 6 || fields[0] != "OK")
  {
    return false;
  }

  out.duration_seconds = toInt(fields[1]);
  out.track_number = toInt(fields[2]);
  out.song_name = Proto::decodeField(fields[3]);
  out.artist_name = Proto::decodeField(fields[4]);
  out.album_name = Proto::decodeField(fields[5]);

  return true;
}

bool Session::art(const std::string &path, Art::ImageData &out)
{
  std::lock_guard<std::mutex> guard(lock_);

  std::string response;
  if (!exchange("ART " + Proto::encodeField(path), response))
  {
    return false;
  }

  std::vector<unsigned char> payload;
  bool had_payload = false;

  if (!readPayload(response, payload, had_payload))
  {
    return false;
  }

  if (!had_payload || payload.empty())
  {
    return false;
  }

  return Art::LoadImageMemory(out, payload.data(), payload.size());
}

bool Session::albums(std::vector<AlbumSummary> &out)
{
  std::lock_guard<std::mutex> guard(lock_);
  out.clear();

  std::string response;
  if (!exchange("ALBUMS", response))
  {
    return false;
  }

  const auto head = Proto::splitFields(response, 2);
  if (head.size() < 2 || head[0] != "OK")
  {
    return false;
  }

  uint64_t count = 0;
  if (!parseU64(head[1], count))
  {
    return abandon();
  }

  out.reserve(static_cast<size_t>(count));
  for (uint64_t i = 0; i < count; ++i)
  {
    std::string row;
    if (!stream_->readLine(row))
    {
      return false;
    }

    const auto fields = Proto::splitFields(row, 7);
    if (fields.size() < 7 || fields[0] != "A")
    {
      return abandon();
    }

    AlbumSummary album;
    album.year = toInt(fields[1]);
    album.track_count = toInt(fields[2]);
    album.total_seconds = toInt(fields[3]);
    album.artist = Proto::decodeField(fields[4]);
    album.title = Proto::decodeField(fields[5]);
    album.directory = Proto::decodeField(fields[6]);
    out.push_back(std::move(album));
  }

  return true;
}

bool Session::albumTracks(int index, std::vector<TrackSummary> &out)
{
  std::lock_guard<std::mutex> guard(lock_);
  out.clear();

  std::string response;
  if (!exchange("ALBUM " + std::to_string(index), response))
  {
    return false;
  }

  const auto head = Proto::splitFields(response, 2);
  if (head.size() < 2 || head[0] != "OK")
  {
    return false;
  }

  uint64_t count = 0;
  if (!parseU64(head[1], count))
  {
    return abandon();
  }

  out.reserve(static_cast<size_t>(count));
  for (uint64_t i = 0; i < count; ++i)
  {
    std::string row;
    if (!stream_->readLine(row))
    {
      return false;
    }

    const auto fields = Proto::splitFields(row, 6);
    if (fields.size() < 6 || fields[0] != "T")
    {
      return abandon();
    }

    TrackSummary track;
    track.track_number = toInt(fields[1]);
    track.duration_seconds = toInt(fields[2]);
    track.title = Proto::decodeField(fields[3]);
    track.artist = Proto::decodeField(fields[4]);
    track.path = Proto::decodeField(fields[5]);
    out.push_back(std::move(track));
  }

  return true;
}

bool Session::startScan()
{
  std::lock_guard<std::mutex> guard(lock_);
  std::string response;

  return exchange("SCAN", response) && response.rfind("OK", 0) == 0;
}

bool Session::status(bool &scanning, int &done, int &total, int &album_count)
{
  std::lock_guard<std::mutex> guard(lock_);

  std::string response;
  if (!exchange("STATUS", response))
  {
    return false;
  }

  const auto fields = Proto::splitFields(response, 5);
  if (fields.size() < 5 || fields[0] != "OK")
  {
    return false;
  }

  scanning = fields[1] == "1";
  done = toInt(fields[2]);
  total = toInt(fields[3]);
  album_count = toInt(fields[4]);

  return true;
}

bool Session::open(const std::string &path, uint32_t &handle, uint64_t &size)
{
  std::lock_guard<std::mutex> guard(lock_);

  std::string response;
  if (!exchange("OPEN " + Proto::encodeField(path), response))
  {
    return false;
  }

  const auto fields = Proto::splitFields(response, 3);
  if (fields.size() < 3 || fields[0] != "OK")
  {
    return false;
  }

  uint64_t id = 0;
  if (!parseU64(fields[1], id) || !parseU64(fields[2], size))
  {
    return false;
  }

  handle = static_cast<uint32_t>(id);

  return true;
}

bool Session::read(uint32_t handle, uint64_t offset, size_t length, std::vector<unsigned char> &out)
{
  std::lock_guard<std::mutex> guard(lock_);

  const size_t clamped = std::min(length, Proto::kMaxChunk);
  std::string response;

  if (!exchange("READ " + std::to_string(handle) + " " + std::to_string(offset) + " " +
                    std::to_string(clamped),
                response))
  {
    return false;
  }

  bool had_payload = false;
  if (!readPayload(response, out, had_payload))
  {
    return false;
  }

  return had_payload;
}

bool Session::closeHandle(uint32_t handle)
{
  std::lock_guard<std::mutex> guard(lock_);
  std::string response;

  return exchange("CLOSE " + std::to_string(handle), response) && response.rfind("OK", 0) == 0;
}

// ---------------------------------------------------------------------------
// Vfs
// ---------------------------------------------------------------------------

std::string Vfs::url(const std::string &wire_path)
{
  return std::string(kScheme) + wire_path;
}

bool Vfs::parse(const char *uri, std::string &wire_path)
{
  if (uri == nullptr)
  {
    return false;
  }

  const size_t prefix = std::strlen(kScheme);
  if (std::strncmp(uri, kScheme, prefix) != 0)
  {
    return false;
  }

  wire_path = uri + prefix;
  return true;
}

namespace
{

/// One open remote file, with the read-ahead window that keeps the decoder
/// from issuing an RPC per 4 KiB read.
struct RemoteFile
{
  Session *session = nullptr;
  uint32_t handle = 0;
  uint64_t size = 0;
  uint64_t cursor = 0; ///< logical file position miniaudio believes in

  std::vector<unsigned char> buffer;
  uint64_t buffer_start = 0; ///< file offset of buffer[0]

  bool contains(uint64_t offset) const
  {
    return offset >= buffer_start && offset < buffer_start + buffer.size();
  }
};

} // namespace

struct VfsState
{
  // ma_vfs_callbacks must be first: miniaudio casts a ma_vfs* straight to the
  // callbacks struct, the same C-style inheritance the tap node uses.
  ma_vfs_callbacks callbacks{};
  Session *session = nullptr;
  ma_default_vfs fallback{};
};

namespace
{

VfsState *implOf(ma_vfs *vfs)
{
  return reinterpret_cast<VfsState *>(vfs);
}

RemoteFile *fileOf(ma_vfs_file file)
{
  return reinterpret_cast<RemoteFile *>(file);
}

// Open remote handles, so a fallback (local) handle is never mistaken for a
// RemoteFile*. miniaudio hands back opaque pointers from both VFSs.
std::mutex g_handles_lock;
std::vector<RemoteFile *> g_handles;

bool isRemoteHandle(ma_vfs_file file)
{
  std::lock_guard<std::mutex> guard(g_handles_lock);
  return std::find(g_handles.begin(), g_handles.end(), reinterpret_cast<RemoteFile *>(file)) !=
         g_handles.end();
}

ma_result vfsOpen(ma_vfs *vfs, const char *pFilePath, ma_uint32 openMode, ma_vfs_file *pFile)
{
  VfsState *impl = implOf(vfs);

  std::string wire;
  if (!Vfs::parse(pFilePath, wire))
  {
    // Not ours: local files must keep working while a session is attached.
    return ma_default_vfs_init(&impl->fallback, nullptr) == MA_SUCCESS
               ? ma_vfs_open(
                     reinterpret_cast<ma_vfs *>(&impl->fallback), pFilePath, openMode, pFile)
               : MA_ERROR;
  }

  if ((openMode & MA_OPEN_MODE_WRITE) != 0)
  {
    return MA_NOT_IMPLEMENTED; // the library is read-only over the wire
  }

  auto remote = std::make_unique<RemoteFile>();
  remote->session = impl->session;

  if (remote->session == nullptr || !remote->session->open(wire, remote->handle, remote->size))
  {
    return MA_DOES_NOT_EXIST;
  }

  remote->buffer.reserve(Proto::kReadAhead);
  RemoteFile *raw = remote.release();
  {
    std::lock_guard<std::mutex> guard(g_handles_lock);
    g_handles.push_back(raw);
  }

  *pFile = reinterpret_cast<ma_vfs_file>(raw);
  return MA_SUCCESS;
}

ma_result vfsOpenW(ma_vfs *, const wchar_t *, ma_uint32, ma_vfs_file *)
{
  return MA_NOT_IMPLEMENTED;
}

ma_result vfsClose(ma_vfs *vfs, ma_vfs_file file)
{
  if (!isRemoteHandle(file))
  {
    return ma_vfs_close(reinterpret_cast<ma_vfs *>(&implOf(vfs)->fallback), file);
  }

  RemoteFile *remote = fileOf(file);
  if (remote->session != nullptr)
  {
    remote->session->closeHandle(remote->handle);
  }

  {
    std::lock_guard<std::mutex> guard(g_handles_lock);
    g_handles.erase(std::remove(g_handles.begin(), g_handles.end(), remote), g_handles.end());
  }

  delete remote;
  return MA_SUCCESS;
}

ma_result vfsRead(ma_vfs *vfs, ma_vfs_file file, void *pDst, size_t sizeInBytes, size_t *pBytesRead)
{
  if (!isRemoteHandle(file))
  {
    return ma_vfs_read(
        reinterpret_cast<ma_vfs *>(&implOf(vfs)->fallback), file, pDst, sizeInBytes, pBytesRead);
  }

  RemoteFile *remote = fileOf(file);
  auto *out = static_cast<unsigned char *>(pDst);
  size_t done = 0;

  while (done < sizeInBytes)
  {
    if (remote->cursor >= remote->size)
    {
      break; // EOF
    }

    if (!remote->contains(remote->cursor))
    {
      // Miss: pull a whole chunk starting at the cursor. Decoders read in
      // small bites, so without this every 4 KiB would cost a round trip.
      if (remote->session == nullptr || !remote->session->alive())
      {
        if (pBytesRead != nullptr)
        {
          *pBytesRead = done;
        }

        return MA_ERROR; // losing the network stops playback, never hangs
      }

      if (!remote->session->read(remote->handle, remote->cursor, Proto::kMaxChunk, remote->buffer))
      {
        if (pBytesRead != nullptr)
        {
          *pBytesRead = done;
        }

        return MA_ERROR;
      }

      remote->buffer_start = remote->cursor;
      if (remote->buffer.empty())
      {
        break;
      }
    }

    const uint64_t offset_in_buffer = remote->cursor - remote->buffer_start;
    const size_t available = remote->buffer.size() - static_cast<size_t>(offset_in_buffer);
    const size_t take = std::min(sizeInBytes - done, available);
    std::memcpy(out + done, remote->buffer.data() + offset_in_buffer, take);
    done += take;
    remote->cursor += take;
  }

  if (pBytesRead != nullptr)
  {
    *pBytesRead = done;
  }

  return (done == 0 && sizeInBytes > 0) ? MA_AT_END : MA_SUCCESS;
}

ma_result vfsWrite(ma_vfs *, ma_vfs_file, const void *, size_t, size_t *)
{
  return MA_NOT_IMPLEMENTED;
}

ma_result vfsSeek(ma_vfs *vfs, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin)
{
  if (!isRemoteHandle(file))
  {
    return ma_vfs_seek(reinterpret_cast<ma_vfs *>(&implOf(vfs)->fallback), file, offset, origin);
  }

  RemoteFile *remote = fileOf(file);
  ma_int64 target = 0;

  switch (origin)
  {
  case ma_seek_origin_start:
    target = offset;
    break;
  case ma_seek_origin_current:
    target = static_cast<ma_int64>(remote->cursor) + offset;
    break;
  case ma_seek_origin_end:
  default:
    target = static_cast<ma_int64>(remote->size) + offset;
    break;
  }

  if (target < 0)
  {
    return MA_INVALID_ARGS;
  }

  // Just move the cursor.
  // If the destination is already inside the buffered window the next read costs nothing
  // else it refills on demand.
  remote->cursor = static_cast<uint64_t>(target);

  return MA_SUCCESS;
}

ma_result vfsTell(ma_vfs *vfs, ma_vfs_file file, ma_int64 *pCursor)
{
  if (!isRemoteHandle(file))
  {
    return ma_vfs_tell(reinterpret_cast<ma_vfs *>(&implOf(vfs)->fallback), file, pCursor);
  }

  if (pCursor == nullptr)
  {
    return MA_INVALID_ARGS;
  }

  *pCursor = static_cast<ma_int64>(fileOf(file)->cursor);
  return MA_SUCCESS;
}

ma_result vfsInfo(ma_vfs *vfs, ma_vfs_file file, ma_file_info *pInfo)
{
  if (!isRemoteHandle(file))
  {
    return ma_vfs_info(reinterpret_cast<ma_vfs *>(&implOf(vfs)->fallback), file, pInfo);
  }

  if (pInfo == nullptr)
  {
    return MA_INVALID_ARGS;
  }

  pInfo->sizeInBytes = fileOf(file)->size;
  return MA_SUCCESS;
}

} // namespace

Vfs::Vfs(Session &session) : impl_(std::make_unique<VfsState>())
{
  impl_->session = &session;
  ma_default_vfs_init(&impl_->fallback, nullptr);

  impl_->callbacks.onOpen = vfsOpen;
  impl_->callbacks.onOpenW = vfsOpenW;
  impl_->callbacks.onClose = vfsClose;
  impl_->callbacks.onRead = vfsRead;
  impl_->callbacks.onWrite = vfsWrite;
  impl_->callbacks.onSeek = vfsSeek;
  impl_->callbacks.onTell = vfsTell;
  impl_->callbacks.onInfo = vfsInfo;
}

Vfs::~Vfs() = default;

ma_vfs *Vfs::handle()
{
  return reinterpret_cast<ma_vfs *>(impl_.get());
}

} // namespace Remote
