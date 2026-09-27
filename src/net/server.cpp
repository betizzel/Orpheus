// server.cpp — verb dispatch for orpheusd. See include/proto.hpp for the spec.
#include "server.hpp"

#include "art.hpp"
#include "library.hpp"
#include "util.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <taglib/tvariant.h>

namespace Server
{

volatile std::sig_atomic_t g_stop = 0;

namespace
{

constexpr size_t kMaxOpenHandles = 16;

/// Per-connection state. Handles are deliberately connection-scoped so a
/// disconnect can never leak a descriptor into the next session.
struct ConnState
{
  std::unordered_map<uint32_t, std::ifstream> files;
  std::unordered_map<uint32_t, uint64_t> sizes;
  uint32_t next_handle = 1;

  // Snapshot of the ordering last sent by ALBUMS, so a following ALBUM <index>
  // refers to the same album even if a rescan reorders things underneath.
  std::vector<Library::Album> album_snapshot;
};

bool parseU64(std::string_view text, uint64_t &out)
{
  const auto *begin = text.data();
  const auto *end = begin + text.size();
  const auto result = std::from_chars(begin, end, out);
  return result.ec == std::errc() && result.ptr == end;
}

bool parseU32(std::string_view text, uint32_t &out)
{
  uint64_t wide = 0;
  if (!parseU64(text, wide) || wide > 0xFFFFFFFFull)
    return false;
  out = static_cast<uint32_t>(wide);
  return true;
}

bool sendLine(Proto::Stream &stream, const std::string &line)
{
  std::string framed = line;
  framed.push_back('\n');
  return stream.writeAll(framed.data(), framed.size());
}

bool sendErr(Proto::Stream &stream, std::string_view reason)
{
  return sendLine(stream, "ERR " + Proto::encodeField(reason));
}

bool sendData(Proto::Stream &stream, const void *bytes, size_t count)
{
  if (!sendLine(stream, "DATA " + std::to_string(count)))
    return false;
  return count == 0 || stream.writeAll(bytes, count);
}

/**
 * @brief Resolve a wire path against the root
 *
 * Two checks, both required. sanitizeWirePath() rejects "..", absolute paths
 * and NULs lexically and weakly_canonical() then resolves symlinks so a link
 * *inside* the library pointing at /etc cannot be followed. 
 */
bool resolve(const Options &opts, std::string_view wire, std::filesystem::path &out)
{
  std::string relative;
  if (!Proto::sanitizeWirePath(wire, relative))
    return false;

  std::error_code ec;
  const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(opts.root, ec);
  if (ec)
    return false;

  const std::filesystem::path joined = relative.empty() ? canonical_root : canonical_root / relative;
  const std::filesystem::path real = std::filesystem::weakly_canonical(joined, ec);
  if (ec)
    return false;

  auto root_it = canonical_root.begin();
  auto real_it = real.begin();
  for (; root_it != canonical_root.end(); ++root_it, ++real_it)
  {
    if (real_it == real.end() || *real_it != *root_it)
      return false;
  }

  out = real;
  return true;
}

/// Wire path for an absolute path known to be inside the root.
std::string toWire(const Options &opts, const std::filesystem::path &absolute)
{
  std::error_code ec;
  const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(opts.root, ec);
  const std::filesystem::path rel = std::filesystem::relative(absolute, canonical_root, ec);
  if (ec)
    return {};
  std::string text = rel.generic_string();
  return text == "." ? std::string() : text;
}

/// Raw encoded cover bytes: embedded picture first, then a sibling image file.
/// we don't expand it to the decoded RGBA because it's a lot more data to send.
/// Images over Proto::kMaxChunk are reported as absent: no DATA frame may be
/// larger (see proto.hpp), and it bounds what one request pulls into memory.
bool coverBytes(const std::filesystem::path &song, std::vector<unsigned char> &out)
{
  out.clear();

  TagLib::FileRef ref(song.c_str());
  if (!ref.isNull() && ref.tag() != nullptr)
  {
    const auto pictures = ref.tag()->complexProperties("PICTURE");
    if (!pictures.isEmpty())
    {
      const auto &picture = pictures.front();
      const auto it = picture.find("data");
      if (it != picture.end() && it->second.type() == TagLib::Variant::ByteVector)
      {
        // An oversized embedded picture falls through to the sibling file,
        // which is often a smaller copy of the same cover.
        const TagLib::ByteVector bytes = it->second.value<TagLib::ByteVector>();
        if (!bytes.isEmpty() && bytes.size() <= Proto::kMaxChunk)
        {
          const auto *raw = reinterpret_cast<const unsigned char *>(bytes.data());
          out.assign(raw, raw + bytes.size());
          return true;
        }
      }
    }
  }

  const std::string sibling = Art::ResolveImage(song.string());
  if (sibling == "no image")
    return false;

  std::ifstream file(sibling, std::ios::binary | std::ios::ate);
  if (!file)
    return false;
  const std::streamsize size = file.tellg();
  if (size <= 0 || static_cast<uintmax_t>(size) > Proto::kMaxChunk)
    return false;
  file.seekg(0);
  out.resize(static_cast<size_t>(size));
  return static_cast<bool>(file.read(reinterpret_cast<char *>(out.data()), size));
}

bool handleList(Proto::Stream &stream, const Options &opts, std::string_view arg)
{
  std::filesystem::path dir;
  if (!resolve(opts, arg, dir))
    return sendErr(stream, "path outside root");

  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec))
    return sendErr(stream, "not a directory");

  std::vector<Util::DirEntry> entries;
  if (!Util::listDir(dir, entries))
    return sendErr(stream, "cannot read directory");

  if (!sendLine(stream, "OK " + std::to_string(entries.size())))
    return false;

  for (const auto &entry : entries)
  {
    if (entry.is_dir)
    {
      if (!sendLine(stream, "D " + Proto::encodeField(entry.name)))
        return false;
      continue;
    }

    const std::filesystem::path full = dir / entry.name;
    const uintmax_t size = std::filesystem::file_size(full, ec);
    const auto stamp = std::filesystem::last_write_time(full, ec);
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(stamp.time_since_epoch()).count();
    if (!sendLine(stream, "F " + std::to_string(ec ? 0 : size) + " " + std::to_string(secs) + " " +
                              Proto::encodeField(entry.name)))
      return false;
  }
  return true;
}

bool handleTags(Proto::Stream &stream, const Options &opts, std::string_view arg)
{
  std::filesystem::path file;
  if (!resolve(opts, arg, file))
    return sendErr(stream, "path outside root");

  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec))
    return sendErr(stream, "not a file");

  // Same code path the local player uses, so remote and local metadata agree
  // (including the file-stem fallback for untagged files).
  const SongMetadata meta = Library::loadSongMetadata(file.string(), false);
  return sendLine(stream, "OK " + std::to_string(meta.duration_seconds) + " " +
    std::to_string(meta.track_number) + " " + Proto::encodeField(meta.song_name) + " " +
    Proto::encodeField(meta.artist_name) + " " + Proto::encodeField(meta.album_name));
}

bool handleArt(Proto::Stream &stream, const Options &opts, std::string_view arg)
{
  std::filesystem::path file;
  if (!resolve(opts, arg, file))
    return sendErr(stream, "path outside root");

  std::vector<unsigned char> bytes;
  if (!coverBytes(file, bytes))
    return sendLine(stream, "OK 0");
  return sendData(stream, bytes.data(), bytes.size());
}

bool handleAlbums(Proto::Stream &stream, const Options &opts, Library::Scanner &scanner, ConnState &conn)
{
  conn.album_snapshot = scanner.albums();
  if (!sendLine(stream, "OK " + std::to_string(conn.album_snapshot.size())))
    return false;

  for (const auto &album : conn.album_snapshot)
  {
    if (!sendLine(stream, "A " + std::to_string(album.year) + " " + std::to_string(album.tracks.size()) + " " +
      std::to_string(album.total_seconds) + " " + Proto::encodeField(album.artist) + " " +
      Proto::encodeField(album.title) + " " +
      Proto::encodeField(toWire(opts, album.directory))))
      return false;
  }
  return true;
}

bool handleAlbum(Proto::Stream &stream, const Options &opts, ConnState &conn, std::string_view arg)
{
  uint32_t index = 0;
  if (!parseU32(arg, index) || index >= conn.album_snapshot.size())
    return sendErr(stream, "no such album");

  const Library::Album &album = conn.album_snapshot[index];
  if (!sendLine(stream, "OK " + std::to_string(album.tracks.size())))
    return false;

  for (const auto &track : album.tracks)
  {
    // Scanner tracks carry absolute paths; clients only ever see wire paths.
    if (!sendLine(stream, "T " + std::to_string(track.track_number) + " " +
      std::to_string(track.duration_seconds) + " " + Proto::encodeField(track.title) + " " +
      Proto::encodeField(track.artist) + " " + Proto::encodeField(toWire(opts, track.path))))
      return false;
  }
  return true;
}

bool handleOpen(Proto::Stream &stream, const Options &opts, ConnState &conn, std::string_view arg)
{
  if (conn.files.size() >= kMaxOpenHandles)
    return sendErr(stream, "too many open files");

  std::filesystem::path file;
  if (!resolve(opts, arg, file))
    return sendErr(stream, "path outside root");

  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec))
    return sendErr(stream, "not a file");

  std::ifstream handle(file, std::ios::binary);
  if (!handle)
    return sendErr(stream, "cannot open");

  const uintmax_t size = std::filesystem::file_size(file, ec);
  if (ec)
    return sendErr(stream, "cannot stat");

  const uint32_t id = conn.next_handle++;
  conn.files.emplace(id, std::move(handle));
  conn.sizes.emplace(id, static_cast<uint64_t>(size));
  return sendLine(stream, "OK " + std::to_string(id) + " " + std::to_string(size));
}

bool handleRead(Proto::Stream &stream, ConnState &conn, const std::vector<std::string_view> &fields)
{
  uint32_t id = 0;
  uint64_t offset = 0;
  uint64_t length = 0;
  if (fields.size() < 4 || !parseU32(fields[1], id) || !parseU64(fields[2], offset) || !parseU64(fields[3], length))
    return sendErr(stream, "bad read arguments");
  if (length > Proto::kMaxChunk)
    return sendErr(stream, "chunk too large");

  const auto it = conn.files.find(id);
  if (it == conn.files.end())
    return sendErr(stream, "bad handle");

  const uint64_t size = conn.sizes[id];
  if (offset >= size || length == 0)
    return sendData(stream, nullptr, 0); // past EOF is not an error

  const uint64_t available = std::min<uint64_t>(length, size - offset);
  std::vector<unsigned char> buffer(static_cast<size_t>(available));

  std::ifstream &file = it->second;
  file.clear(); // a previous read may have set eofbit
  file.seekg(static_cast<std::streamoff>(offset));
  file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(available));
  const size_t got = static_cast<size_t>(file.gcount());

  return sendData(stream, buffer.data(), got);
}

bool handleStatus(Proto::Stream &stream, Library::Scanner &scanner)
{
  return sendLine(stream, "OK " + std::string(scanner.isScanning() ? "1" : "0") + " " +
    std::to_string(scanner.filesScanned()) + " " + std::to_string(scanner.filesTotal()) +
    " " + std::to_string(scanner.albumCount()));
}

} // namespace

bool serveConnection(Proto::Stream &stream, const Options &opts)
{
  ConnState conn;
  Library::Scanner scanner;
  if (opts.scan_on_start)
    scanner.start(opts.root);

  std::string line;
  // g_stop is only ever set by orpheusd's SIGINT/SIGTERM handler
  // a signal that lands mid-request is noticed before blocking on the next one.
  while (!g_stop && stream.readLine(line))
  {
    const auto fields = Proto::splitFields(line);
    if (fields.empty())
      continue;

    const std::string_view verb = fields[0];
    const std::string_view arg = fields.size() > 1 ? fields[1] : std::string_view();
    bool ok = true;

    // Every malformed request answers ERR and keeps the connection alive: a
    // buggy client must not be able to take the daemon down.
    if (verb == "HELLO")
    {
      uint32_t version = 0;
      if (!parseU32(arg, version))
        ok = sendErr(stream, "bad version");
      else if (static_cast<int>(version) != Proto::kVersion)
        ok = sendErr(stream, "protocol version mismatch, server speaks " + std::to_string(Proto::kVersion));
      else
        ok = sendLine(stream, "OK " + std::to_string(Proto::kVersion) + " " + Proto::encodeField(opts.label));
    }
    else if (verb == "LIST")
      ok = handleList(stream, opts, Proto::decodeField(arg));
    else if (verb == "TAGS")
      ok = handleTags(stream, opts, Proto::decodeField(arg));
    else if (verb == "ART")
      ok = handleArt(stream, opts, Proto::decodeField(arg));
    else if (verb == "ALBUMS")
      ok = handleAlbums(stream, opts, scanner, conn);
    else if (verb == "ALBUM")
      ok = handleAlbum(stream, opts, conn, arg);
    else if (verb == "SCAN")
    {
      scanner.start(opts.root, true);
      ok = sendLine(stream, "OK");
    }
    else if (verb == "STATUS")
      ok = handleStatus(stream, scanner);
    else if (verb == "OPEN")
      ok = handleOpen(stream, opts, conn, Proto::decodeField(arg));
    else if (verb == "READ")
      ok = handleRead(stream, conn, fields);
    else if (verb == "CLOSE")
    {
      uint32_t id = 0;
      if (!parseU32(arg, id) || conn.files.erase(id) == 0)
        ok = sendErr(stream, "bad handle");
      else
      {
        conn.sizes.erase(id);
        ok = sendLine(stream, "OK");
      }
    }
    else if (verb == "BYE")
    {
      sendLine(stream, "OK");
      scanner.cancel();
      return true;
    }
    else
    {
      ok = sendErr(stream, "unknown verb");
    }

    if (!ok)
      break; // write failure: the peer is gone
  }

  scanner.cancel();
  return false;
}

bool serveUnixSocket(const std::filesystem::path &socket_path, const Options &opts)
{
  const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listener < 0)
  {
    Util::errorPrint("socket() failed");
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const std::string path = socket_path.string();
  if (path.size() >= sizeof(addr.sun_path))
  {
    Util::errorPrint("socket path too long: " + path);
    ::close(listener);
    return false;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size());

  ::unlink(path.c_str()); // a stale socket from a crashed run would block bind
  if (::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
  {
    Util::errorPrint("bind() failed on " + path);
    ::close(listener);
    return false;
  }
  if (::listen(listener, 4) != 0)
  {
    Util::errorPrint("listen() failed on " + path);
    ::close(listener);
    ::unlink(path.c_str());
    return false;
  }

  Util::infoPrint("orpheusd listening on " + path);

  while (!g_stop)
  {
    const int client = ::accept(listener, nullptr, nullptr);
    if (client < 0)
    {
      if (errno == EINTR)
        continue; // a signal, not a failure
      break;
    }

    Proto::FdStream stream(client, client, true);
    stream.setInterrupt(&g_stop);
    serveConnection(stream, opts);
  }

  ::close(listener);
  ::unlink(path.c_str());
  return true;
}

} // namespace Server
