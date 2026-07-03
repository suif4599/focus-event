// focus-event-trigger: the user-space half of the split.
//
// Spawned by niri (via `spawn-at-startup`), so it inherits the user's full
// environment — XDG_RUNTIME_DIR, NIRI_SOCKET, PATH — with no munging needed.
// It runs the focus-event engine against `niri msg -j event-stream`, and on
// each rule match sends a SPAWN message over a Unix domain socket to the
// privileged focus-event-executor.
//
// The trigger never runs the configured spawn commands itself; only the
// executor does. That lets the trigger stay user-privilege while still firing
// privileged actions (keyd, pactl on system outputs, etc.).

#include "lib/config.hpp"
#include "lib/engine.hpp"
#include "lib/epoll.hpp"
#include "lib/niri.hpp"
#include "lib/protocol.hpp"
#include "lib/subprocess.hpp"
#include "lib/uds.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// `environ` is declared in <unistd.h> when _GNU_SOURCE is set; we declare it
// explicitly here so we don't have to bring in _GNU_SOURCE. Must live outside
// the anonymous namespace below so it's a true extern reference.
extern "C" char** environ;

namespace {

constexpr const char* DEFAULT_SOCKET = "/run/focus-event.sock";
constexpr const char* DEFAULT_CONFIG_REL = "focus-event/config.kdl";

std::string default_config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return (fs::path(xdg) / DEFAULT_CONFIG_REL).string();
    const char* home = std::getenv("HOME");
    if (home && *home) return (fs::path(home) / ".config" / DEFAULT_CONFIG_REL).string();
    return "config.kdl";
}

void print_usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [--config PATH] [--socket PATH] [--help]\n"
        << "  --config PATH   KDL config file (default: $XDG_CONFIG_HOME/focus-event/config.kdl)\n"
        << "  --socket PATH   Executor socket (default: " << DEFAULT_SOCKET << ")\n"
        << "                  Set --socket '' to run standalone (spawn locally, no executor).\n";
}

// Capture this process's environment as a list of (key, value) pairs.
// Used to ship the trigger's env to the executor so spawn()ed commands
// inherit the user's PATH, XDG_RUNTIME_DIR, NIRI_SOCKET, DBUS address, etc.
// — not the system service's minimal env.
std::vector<std::pair<std::string, std::string>> snapshot_env() {
    std::vector<std::pair<std::string, std::string>> out;
    for (char** e = environ; *e; ++e) {
        std::string entry(*e);
        auto eq = entry.find('=');
        if (eq != std::string::npos) {
            out.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
        }
    }
    return out;
}

// Spawner that forwards argv over a connected UDS to the executor.
// If the connection drops, we attempt to reconnect transparently so the
// user doesn't lose focus events while the executor is being restarted.
class RemoteSpawner : public engine::Spawner {
public:
    explicit RemoteSpawner(std::string socket_path) : socket_path_(std::move(socket_path)) {
        // Connect opportunistically; failure is fine, we'll retry on first spawn.
        try_connect();
    }

    void spawn(const std::vector<std::string>& argv) override {
        if (!ensure_connected()) {
            std::cerr << "trigger: executor unreachable, dropping spawn of `"
                      << (argv.empty() ? "" : argv[0])
                      << "` (last error: " << last_error_ << ")\n";
            return;
        }
        auto frame = protocol::encode_spawn(argv);
        if (!uds::write_all(fd_, frame.data(), frame.size())) {
            std::cerr << "trigger: write to executor failed: "
                      << std::strerror(errno) << ", closing socket\n";
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    bool ensure_connected() {
        if (fd_ >= 0) return true;
        return try_connect();
    }

    bool try_connect() {
        if (socket_path_.empty()) return false;
        try {
            fd_ = uds::connect(socket_path_);
        } catch (const std::exception& e) {
            last_error_ = e.what();
            fd_ = -1;
            return false;
        }
        last_error_.clear();
        // Send hello so the executor's logs associate this connection with us.
        std::string label = "pid=" + std::to_string(::getpid());
        auto hello = protocol::encode_hello(label);
        uds::write_all(fd_, hello.data(), hello.size());

        // Ship our environment so the executor's spawn() runs commands with
        // our PATH / XDG_RUNTIME_DIR / NIRI_SOCKET etc., not the systemd
        // service's minimal env. Send once per connection; env doesn't change
        // during our lifetime.
        auto envframe = protocol::encode_env(snapshot_env());
        uds::write_all(fd_, envframe.data(), envframe.size());
        return fd_ >= 0;
    }

    std::string socket_path_;
    std::string last_error_;
    int fd_ = -1;
};

// Read complete lines from fd, calling on_line for each. Returns false on EOF.
bool drain_lines(int fd, std::string& buf,
                 const std::function<void(std::string_view)>& on_line) {
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
            continue;
        }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        std::cerr << "trigger: read error: " << std::strerror(errno) << "\n";
        return false;
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path = default_config_path();
    std::string socket_path = DEFAULT_SOCKET;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "missing value for " << a << "\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "--config") config_path = next();
        else if (a == "--socket") socket_path = next();
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else { std::cerr << "unknown arg '" << a << "'\n"; print_usage(argv[0]); return 2; }
    }

    // SIGPIPE: writes to a closed executor socket must not kill us.
    ::signal(SIGPIPE, SIG_IGN);

    config::Config cfg;
    try {
        cfg = config::load_file(config_path);
    } catch (const std::exception& e) {
        std::cerr << "trigger: failed to load config from " << config_path
                  << ": " << e.what() << "\n";
        return 1;
    }
    std::cerr << "trigger: loaded " << cfg.rules.size() << " rule(s) from "
              << config_path << "\n";

    // Pick spawner based on whether --socket is empty. Ownership is held by
    // the unique_ptr; the bare pointer is what we hand to the engine.
    std::unique_ptr<engine::LocalSpawner> local_fallback;
    std::unique_ptr<RemoteSpawner> remote;
    engine::Spawner* spawner = nullptr;
    if (socket_path.empty()) {
        local_fallback = std::make_unique<engine::LocalSpawner>();
        spawner = local_fallback.get();
        std::cerr << "trigger: socket path empty, spawning locally (standalone mode)\n";
    } else {
        remote = std::make_unique<RemoteSpawner>(socket_path);
        spawner = remote.get();
        std::cerr << "trigger: forwarding spawns to executor at " << socket_path << "\n";
    }

    engine::Engine eng(std::move(cfg), *spawner);
    try {
        eng.bootstrap();
    } catch (const std::exception& e) {
        std::cerr << "trigger: bootstrap failed: " << e.what() << "\n";
        return 1;
    }
    std::cerr << "trigger: bootstrap ok, " << eng.cache_size() << " window(s) cached\n";

    subprocess::PipeResult pipe;
    try {
        pipe = subprocess::launch_pipe_stdout({"niri", "msg", "-j", "event-stream"});
    } catch (const std::exception& e) {
        std::cerr << "trigger: failed to start `niri msg -j event-stream`: "
                  << e.what() << "\n";
        return 1;
    }
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
            std::cerr << "trigger: niri event stream closed unexpectedly\n";
            stream_failed = true;
            loop.stop();
        }
    });

    try { loop.run(); }
    catch (const std::exception& e) {
        std::cerr << "trigger: epoll loop error: " << e.what() << "\n";
        ::close(pipe.read_fd);
        return 1;
    }

    ::close(pipe.read_fd);
    if (pipe.pid > 0) {
        int status = 0;
        while (::waitpid(pipe.pid, &status, 0) < 0 && errno == EINTR) { }
    }
    return stream_failed ? 1 : 0;
}
