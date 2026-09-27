// server.hpp: orpheusd, the machine-side half of remote playback.
#pragma once

#include "proto.hpp"
#include <csignal>
#include <filesystem>
#include <string>

namespace Server
{

struct Options
{
  std::filesystem::path root; ///< music root; nothing outside it is reachable
  bool scan_on_start = true;  ///< build the album index at startup
  std::string label;          ///< reported in HELLO; defaults to the hostname
};

/**
 * @brief Serve one client to completion.
 * @return true on a clean BYE, false when the peer vanished or framing broke.
 */
bool serveConnection(Proto::Stream &stream, const Options &opts);

/**
 * @brief Listen on a Unix socket and serve clients one at a time until a
 *        termination signal arrives. Unlinks the socket on the way out.
 */
bool serveUnixSocket(const std::filesystem::path &socket_path, const Options &opts);

/// Set by the signal handler in orpheusd.cpp; checked by the accept loop.
extern volatile std::sig_atomic_t g_stop;

} // namespace Server
