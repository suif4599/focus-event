// focus-event: react to niri window focus changes by running configured actions.
//
// Usage:
//   focus-event [--config PATH]
//
// Default config path: $XDG_CONFIG_HOME/focus-event/config.kdl, falling back to
// ~/.config/focus-event/config.kdl.

#include "config.hpp"
#include "engine.hpp"
#include "epoll.hpp"
#include "niri.hpp"
#include "subprocess.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* DEFAULT_CONFIG_REL = "focus-event/config.kdl";

std::string default_config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        fs::path p = fs::path(xdg) / DEFAULT_CONFIG_REL;
        return p.string();
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        fs::path p = fs::path(home) / ".config" / DEFAULT_CONFIG_REL;
        return p.string();
    }
    return "config.kdl";
}

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [--config PATH] [--help]\n";
}

// Read complete lines from fd, calling `on_line` for each. Partial trailing
// data is held in `buf` between calls. Returns false on EOF, true otherwise.
bool drain_lines(int fd, std::string& buf, std::function<void(std::string_view)> on_line) {
    char tmp[16384];
    while (true) {
        ssize_t n = ::read(fd, tmp, sizeof(tmp));
        if (n > 0) {
            buf.append(tmp, static_cast<std::size_t>(n));
            std::size_t start = 0;
            while (true) {
                std::size_t nl = buf.find('\n', start);
                if (nl == std::string::npos) break;
                on_line(std::string_view(buf.data() + start, nl - start));
                start = nl + 1;
            }
            if (start > 0) buf.erase(0, start);
            // We may have more data to read; loop until EAGAIN.
            continue;
        }
        if (n == 0) return false; // EOF
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        std::cerr << "focus-event: read error: " << std::strerror(errno) << "\n";
        return false;
    }
}

// We deliberately do NOT install a SIGCHLD reaper. Such a handler would race
// with the synchronous waitpid() inside subprocess::run_capture_stdout()
// (which Engine::refresh_from_niri() calls on cache misses), causing
// "waitpid: No child processes" (ECHILD) errors. Instead:
//   - subprocess::run_capture_stdout() and launch_pipe_stdout() reap their
//     own children explicitly.
//   - subprocess::spawn_detached() double-forks so the actual command is
//     reparented to PID 1, which reaps it for free.
//   - main() reaps the long-running niri-msg child when its stream closes.

} // namespace

int main(int argc, char** argv) {
    std::string config_path = default_config_path();

    static option long_opts[] = {
        {"config", required_argument, nullptr, 'c'},
        {"help",   no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };
    int opt;
    while ((opt = ::getopt_long(argc, argv, "c:h", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'c': config_path = optarg; break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    config::Config cfg;
    try {
        cfg = config::load_file(config_path);
    } catch (const std::exception& e) {
        std::cerr << "focus-event: failed to load config from " << config_path
                  << ": " << e.what() << "\n";
        return 1;
    }
    std::cerr << "focus-event: loaded " << cfg.rules.size() << " rule(s) from "
              << config_path << "\n";

    engine::Engine eng(std::move(cfg));
    try {
        eng.bootstrap();
    } catch (const std::exception& e) {
        std::cerr << "focus-event: bootstrap failed: " << e.what() << "\n";
        return 1;
    }
    std::cerr << "focus-event: bootstrap ok, " << eng.cache_size() << " window(s) cached\n";

    // Launch niri msg -j event-stream.
    subprocess::PipeResult pipe;
    try {
        pipe = subprocess::launch_pipe_stdout({"niri", "msg", "-j", "event-stream"});
    } catch (const std::exception& e) {
        std::cerr << "focus-event: failed to start `niri msg -j event-stream`: "
                  << e.what() << "\n";
        return 1;
    }

    // Set the read fd non-blocking so drain_lines can EAGAIN cleanly.
    int flags = ::fcntl(pipe.read_fd, F_GETFL, 0);
    ::fcntl(pipe.read_fd, F_SETFL, flags | O_NONBLOCK);

    epoll::Loop loop;
    std::string line_buf;
    bool stream_failed = false;
    loop.add(pipe.read_fd, [&](int fd) {
        bool ok = drain_lines(fd, line_buf, [&](std::string_view line) {
            if (line.empty()) return;
            niri::Event ev = niri::parse_event(line);
            eng.handle_event(ev);
        });
        if (!ok) {
            // Treat premature EOF as a failure so systemd's Restart=on-failure
            // kicks in. niri msg normally runs forever; EOF means either niri
            // crashed, our IPC was rejected, or `niri msg` couldn't even start
            // (missing PATH, socket vanished, etc.). Returning non-zero lets
            // the unit cycle back to a clean state once niri is reachable
            // again instead of silently going inactive.
            std::cerr << "focus-event: niri event stream closed unexpectedly\n";
            stream_failed = true;
            loop.stop();
        }
    });

    try {
        loop.run();
    } catch (const std::exception& e) {
        std::cerr << "focus-event: epoll loop error: " << e.what() << "\n";
        ::close(pipe.read_fd);
        return 1;
    }

    ::close(pipe.read_fd);

    // Reap the long-running niri-msg child. We didn't waitpid it earlier
    // because the loop only exits on EOF or stop(); now we own it again.
    if (pipe.pid > 0) {
        int status = 0;
        while (::waitpid(pipe.pid, &status, 0) < 0 && errno == EINTR) { }
    }

    return stream_failed ? 1 : 0;
}
