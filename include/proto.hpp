// proto.hpp — the Orpheus remote protocol.
//
// A client (the TUI) drives a server (orpheusd) that lives wherever the music
// files are. The server never decodes or plays anything: it serves directory
// listings, tags, cover art and raw byte ranges. Audio is decoded and played
// on the client, so the album art renderer and the FFT visualizer keep working
// on local PCM exactly as they do for local files.
//
// TRANSPORT
// The canonical transport is `ssh <host> orpheusd --stdio`: the server speaks
// the protocol on stdin/stdout of a process SSH spawned for us. That means no
// listening port, no authentication to invent, no TLS, and the user's existing
// keys and ~/.ssh/config apply unchanged. A Unix socket (`--socket <path>`) is
// supported for the same-machine case.
//
// IMPORTANT: orpheusd MUST NOT write anything to stdout that is not protocol
// traffic. Util::debugPrint and friends write to stdout, so the server has to
// redirect them (to stderr or a log) before serving on --stdio.
//
// FRAMING
// Requests and responses are single '\n'-terminated ASCII lines of
// space-separated fields. Free-text fields (file names, tag values) are
// percent-encoded with encodeField() so a name can never contain a space,
// CR, LF or '%' on the wire.
//
//   request    "<VERB>[ <field>...]\n"
//   response   "OK[ <field>...]\n"  |  "ERR <encoded-message>\n"
//
// Bulk binary payloads are introduced by their own response line and are NOT
// newline terminated:
//
//   "DATA <nbytes>\n" followed by exactly <nbytes> raw octets
//
// Row responses answer "OK <count>\n" and then emit exactly <count> further
// lines, each in the shape documented per verb below.
//
// The protocol is strictly request/response and single-threaded per
// connection: the client sends one request and reads its complete response
// (including any payload) before sending the next.
//
// PATHS
// Every path on the wire is RELATIVE to the server's music root and uses '/'
// separators. The server MUST reject any path that escapes the root once
// lexically normalised (a "..", an absolute path, or a symlink pointing out).
// The empty string denotes the root itself.
//
// VERBS
//   HELLO <version>
//     -> OK <version> <encoded-root-label>
//     Version mismatch is a hard ERR; there is no negotiation yet.
//
//   LIST <path>
//     -> OK <count>, then <count> rows:
//          "D <encoded-name>"                directory
//          "F <size> <mtime> <encoded-name>" playable file or playlist
//     Rows arrive sorted: directories first, then files, both naturally
//     ordered, matching Util::listDir so local and remote browsing agree.
//
//   TAGS <path>
//     -> OK <duration-seconds> <track-number> <encoded-title> <encoded-artist>
//        <encoded-album>
//     Untagged files still answer OK, with the file stem as the title and
//     "Unknown Artist" as the artist, mirroring Library::loadSongMetadata.
//
//   ART <path>
//     -> DATA <nbytes>   embedded picture, else a sibling cover image
//     -> OK 0            nothing found
//
//   ALBUMS
//     -> OK <count>, then <count> rows:
//          "A <year> <track-count> <total-seconds> <encoded-artist>
//             <encoded-title> <encoded-dir>"
//     Served from the server's Library::Scanner, so a big remote library is
//     indexed once on the machine that owns the disks.
//
//   ALBUM <index>
//     -> OK <count>, then <count> rows:
//          "T <track-number> <duration-seconds> <encoded-title>
//             <encoded-artist> <encoded-path>"
//     <index> indexes the most recent ALBUMS response.
//
//   SCAN
//     -> OK          start (or restart) a library scan, returns immediately
//
//   STATUS
//     -> OK <scanning:0|1> <files-done> <files-total> <album-count>
//
//   OPEN <path>
//     -> OK <handle> <size>
//     Opens a file for byte-range reads. Handles are per-connection.
//
//   READ <handle> <offset> <length>
//     -> DATA <nbytes>
//     <nbytes> may be less than <length> at end of file, and 0 exactly at EOF.
//     <length> MUST NOT exceed kMaxChunk.
//
//   CLOSE <handle>
//     -> OK
//
//   BYE
//     -> OK, then the server closes the connection.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Proto
{

/// Bumped on any incompatible wire change. HELLO fails across versions.
inline constexpr int kVersion = 1;

/// Longest accepted protocol line, payloads excluded.
inline constexpr size_t kMaxLine = 16 * 1024;

/// Largest single READ. Bounds the server's scratch buffer.
inline constexpr size_t kMaxChunk = 1u << 20; // 1 MiB

/// Default read-ahead the client keeps in front of the decoder.
inline constexpr size_t kReadAhead = 2u << 20; // 2 MiB

/**
 * @brief Byte transport underneath the protocol: an SSH pipe pair, a Unix
 * socket, or a plain fd pair. All calls are blocking.
 */
class Stream
{
public:
  virtual ~Stream() = default;

  /// Read exactly n bytes. False on EOF or error (partial data is discarded).
  virtual bool readExact(void *dst, size_t n) = 0;
  /// Write all n bytes. False on error.
  virtual bool writeAll(const void *src, size_t n) = 0;
  /// Read one '\n'-terminated line, newline stripped. False on EOF/overlong.
  virtual bool readLine(std::string &out) = 0;
  /// True while the transport is usable.
  virtual bool good() const = 0;
  virtual void close() = 0;
};

/**
 * @brief Percent-encode a free-text field so it survives space-separated
 * framing. Encodes '%', ' ', CR, LF and every other control character.
 */
std::string encodeField(std::string_view raw);

/**
 * @brief Inverse of encodeField. Malformed escapes decode to themselves rather
 * than throwing; a hostile peer must not be able to abort the process.
 */
std::string decodeField(std::string_view wire);

/**
 * @brief Split a protocol line into at most `max_fields` space-separated
 * fields. Fields are returned still encoded; run decodeField() on the ones
 * that are free text.
 */
std::vector<std::string_view> splitFields(std::string_view line, size_t max_fields = 16);

/**
 * @brief Lexically normalise a wire path and confirm it stays inside the root.
 * @param relative  path as received from the client
 * @param[out] out  normalised relative path ("" for the root itself)
 * @return false when the path escapes, is absolute, or contains a NUL.
 *         Callers MUST treat false as a protocol error, never as "empty".
 */
bool sanitizeWirePath(std::string_view relative, std::string &out);

/**
 * @brief Stream over a read fd and a write fd (one fd for a socket, two for a
 * pipe pair). Reads are buffered, so these calls must not be mixed with
 * direct access to the same descriptor.
 */
class FdStream final : public Stream
{
public:
  /// Closes the descriptors on destruction when `owns` is set.
  FdStream(int read_fd, int write_fd, bool owns = true);
  ~FdStream() override;
  FdStream(const FdStream &) = delete;
  FdStream &operator=(const FdStream &) = delete;

  bool readExact(void *dst, size_t n) override;
  bool writeAll(const void *src, size_t n) override;
  bool readLine(std::string &out) override;
  bool good() const override;
  void close() override;

private:
  bool fill();

  int read_fd_;
  int write_fd_;
  bool owns_;
  bool good_ = true;
  std::vector<char> buf_;
  size_t head_ = 0;
  size_t tail_ = 0;
};

} // namespace Proto
