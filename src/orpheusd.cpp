// orpheusd: serves a music library to a remote Orpheus client.
//
// The daemon never decodes or plays anything. It hands out directory
// listings, tags, cover-art bytes and raw byte ranges; the client does all
// decoding so audio comes out of the machine the listener is sitting at.
//
// Usual invocation is not by hand: the client runs
//     ssh <host> orpheusd --stdio
// and talks the protocol over that process's stdin/stdout.

#include "proto.hpp"
#include "server.hpp"
#include "util.hpp"

#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

namespace
{

extern "C" void onSignal(int)
{
  // Only async-signal-safe work belongs in a handler. The blocking accept()
  // or read() this interrupts sees EINTR and the loops notice g_stop;
  // serveUnixSocket unlinks the socket on its way out.
  Server::g_stop = 1;
}

/// Install without SA_RESTART. std::signal on glibc restarts interrupted
/// syscalls, so a blocking accept()/read() would never notice g_stop.
void installStopHandler(int signal_number)
{
  struct sigaction action{};
  action.sa_handler = onSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  ::sigaction(signal_number, &action, nullptr);
}

void usage()
{
  std::cerr << "orpheusd - Orpheus remote library server\n\n"
               "  --stdio             serve the protocol on stdin/stdout (the ssh case)\n"
               "  --socket <path>     serve on a Unix socket\n"
               "  --root <dir>        music root to serve (default ~/Music)\n"
               "  --label <name>      name reported to clients (default: hostname)\n"
               "  --no-scan           don't build the album index at startup\n"
               "  --help              this text\n\n"
               "Client side:  orpheus --remote <host>\n";
}

std::string defaultLabel()
{
  char host[256] = {0};
  if (::gethostname(host, sizeof(host) - 1) == 0 && host[0] != '\0')
  {
    return host;
  }

  return "orpheusd";
}

} // namespace

int main(int argc, char **argv)
{
  Server::Options opts;
  opts.root = Util::expandHome("~/Music");
  opts.label = defaultLabel();

  bool use_stdio = false;
  std::string socket_path;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    const bool has_value = (i + 1) < argc;

    if (arg == "--stdio")
    {
      use_stdio = true;
    }
    else if (arg == "--socket" && has_value)
    {
      socket_path = argv[++i];
    }
    else if (arg == "--root" && has_value)
    {
      opts.root = Util::expandHome(argv[++i]);
    }
    else if (arg == "--label" && has_value)
    {
      opts.label = argv[++i];
    }
    else if (arg == "--no-scan")
    {
      opts.scan_on_start = false;
    }
    else if (arg == "--help" || arg == "-h")
    {
      usage();
      return 0;
    }
    else
    {
      std::cerr << "orpheusd: unknown argument '" << arg << "'\n\n";
      usage();

      return 2;
    }
  }

  if (use_stdio && !socket_path.empty())
  {
    std::cerr << "orpheusd: --stdio and --socket are mutually exclusive\n\n";
    usage();

    return 2;
  }

  if (!use_stdio && socket_path.empty())
  {
    std::cerr << "orpheusd: pick one of --stdio or --socket <path>\n\n";
    usage();

    return 2;
  }

  // A client vanishing mid-READ must not kill the daemon.
  std::signal(SIGPIPE, SIG_IGN);
  installStopHandler(SIGINT);
  installStopHandler(SIGTERM);

  std::error_code ec;
  if (!std::filesystem::is_directory(opts.root, ec))
  {
    std::cerr << "orpheusd: music root is not a directory: " << opts.root.string() << "\n";
    return 1;
  }

  if (use_stdio)
  {
    // CRITICAL: Util::debugPrint and infoPrint write to std::cout, and on
    // --stdio std::cout IS the protocol channel. One stray log line (the
    // library scanner emits several at startup) desynchronises the framing
    // and every subsequent response is garbage, which is miserable to debug.
    // Point cout at stderr for the lifetime of the session; SSH forwards
    // stderr to the client's terminal/log where it belongs.
    std::streambuf *saved = std::cout.rdbuf(std::cerr.rdbuf());

    Proto::FdStream stream(STDIN_FILENO, STDOUT_FILENO, /*owns=*/false);
    stream.setInterrupt(&Server::g_stop);
    Server::serveConnection(stream, opts);

    std::cout.rdbuf(saved);
    return 0;
  }

  return Server::serveUnixSocket(socket_path, opts) ? 0 : 1;
}
