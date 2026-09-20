// Orpheus — a terminal music player.
//
// main() owns process-level setup only: logging redirection, configuration,
// choosing a music source (local disk or a remote orpheusd over SSH), ncurses
// initialisation, and handing control to UIManager.

#include "config.hpp"
#include "log.hpp"
#include "remote.hpp"
#include "source.hpp"
#include "ui.hpp"
#include "util.hpp"

#include <clocale>
#include <iostream>
#include <memory>
#include <ncurses.h>
#include <string>
#include <vector>

namespace
{

void usage()
{
  std::cerr << "orpheus - a terminal music player\n\n"
               "  --remote <host>        play a library served by orpheusd on <host>, over ssh\n"
               "  --socket <path>        attach to a local orpheusd Unix socket\n"
               "  --remote-cmd <cmd>     command run on the remote host (default: orpheusd --stdio)\n"
               "  --ssh-opt <arg>        extra argument for ssh; repeatable\n"
               "                         e.g. --ssh-opt -p --ssh-opt 2222, or --ssh-opt -Jbastion\n"
               "  --music-dir <dir>      local music root, overriding the config\n"
               "  --help                 this text\n\n"
               "Audio always plays on THIS machine; a remote host only serves files.\n";
}

} // namespace

int main(int argc, char **argv)
{
  // Util::*Print writes to cout/cerr; ncurses owns the terminal, so both are
  // redirected into a log file for the lifetime of the program.
  FileLogger logger("orpheus_debug.log");
  std::streambuf *old_cout = std::cout.rdbuf(logger.rdbuf());
  std::streambuf *old_cerr = std::cerr.rdbuf(logger.rdbuf());

  std::string remote_host;
  std::string socket_path;
  std::string remote_cmd = "orpheusd --stdio";
  std::string music_dir_override;
  std::vector<std::string> ssh_opts;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    const bool has_value = (i + 1) < argc;

    if (arg == "--remote" && has_value)
      remote_host = argv[++i];
    else if (arg == "--socket" && has_value)
      socket_path = argv[++i];
    else if (arg == "--remote-cmd" && has_value)
      remote_cmd = argv[++i];
    else if (arg == "--ssh-opt" && has_value)
      ssh_opts.emplace_back(argv[++i]);
    else if (arg == "--music-dir" && has_value)
      music_dir_override = argv[++i];
    else if (arg == "--help" || arg == "-h")
    {
      std::cout.rdbuf(old_cout);
      std::cerr.rdbuf(old_cerr);
      usage();
      return 0;
    }
    else
    {
      std::cout.rdbuf(old_cout);
      std::cerr.rdbuf(old_cerr);
      std::cerr << "orpheus: unknown argument '" << arg << "'\n\n";
      usage();
      return 2;
    }
  }

  if (Config::writeDefaultConfig())
    Util::infoPrint("Wrote a starter config to " + Config::configPath().string());

  Config::Settings cfg = Config::load();
  Util::debugPrint("Config: " + Config::lastStatus());

  // A --remote/--socket flag wins; otherwise the config may name a host.
  if (remote_host.empty() && socket_path.empty() && !cfg.remote_host.empty())
  {
    remote_host = cfg.remote_host;
    if (remote_cmd == "orpheusd --stdio" && !cfg.remote_command.empty())
      remote_cmd = cfg.remote_command;
  }

  // The session must outlive the UI: the VFS and the remote provider both
  // borrow it. Declared here so it is destroyed last.
  std::unique_ptr<Remote::Session> session;
  std::unique_ptr<Remote::Vfs> vfs;
  std::unique_ptr<Source::Provider> source;

  if (!remote_host.empty() || !socket_path.empty())
  {
    std::string error;
    session = remote_host.empty() ? Remote::Session::connectUnix(socket_path, error)
                                  : Remote::Session::connectSsh(remote_host, remote_cmd, ssh_opts, error);
    if (!session)
    {
      // Connection problems are the single most likely failure here, and the
      // user cannot read the log while ncurses owns the screen. Report before
      // starting the TUI and exit.
      std::cout.rdbuf(old_cout);
      std::cerr.rdbuf(old_cerr);
      std::cerr << "orpheus: " << error << "\n";
      return 1;
    }

    Util::infoPrint("Connected to " + session->describe() + " (" + session->label() + ")");
    vfs = std::make_unique<Remote::Vfs>(*session);
    source = Source::makeRemote(*session);
  }
  else
  {
    const std::string music_dir = music_dir_override.empty() ? cfg.music_dir : music_dir_override;
    const std::filesystem::path music_path = Util::expandHome(music_dir);
    Util::debugPrint("Music root: " + music_path.string());
    source = Source::makeLocal(music_path.string());
  }

  setlocale(LC_ALL, ""); // wide-char output: album art, box drawing, unicode tags
  initscr();
  set_escdelay(0);       // no delay after <Esc>
  raw();
  keypad(stdscr, TRUE);
  noecho();
  curs_set(0);

  {
    UIManager ui_manager;
    ui_manager.init(cfg, std::move(source), vfs ? vfs->handle() : nullptr);
    ui_manager.run();
  } // destructor drops the source and ends ncurses before cout is restored

  vfs.reset();
  session.reset();

  std::cout.rdbuf(old_cout);
  std::cerr.rdbuf(old_cerr);
  return 0;
}
