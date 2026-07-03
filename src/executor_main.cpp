// focus-event-executor: privileged system service that spawns commands on
// behalf of focus-event-trigger.
//
// Listens on a Unix domain socket (default /run/focus-event.sock). For each
// connecting client:
//   1. Reads SO_PEERCRED to get the peer's pid.
//   2. Resolves /proc/<pid>/exe to find the binary the peer is currently
//      running, and compares it against --expected-trigger (the store path
//      of the trusted trigger binary).
//   3. Only if the binaries match does it read frames and act on them.
//
// Why binary-identity rather than uid-allowlist:
//   - The trigger is the only program that should ever issue spawn commands.
//     Authorising by binary identity ties trust to the actual code, not to
//     whichever user happens to run it.
//   - The expected path lives in /nix/store, whose prefix already encodes a
//     content hash, so this is equivalent to verifying a SHA256 of the binary
//     without needing a hashing dependency.
//   - Any local user can connect (the socket is mode 0666 by default); only
//     the trusted trigger binary gets past the auth check.
//
// The daemon is intentionally stateless: it does not load any config and does
// not know about windows or rules. All policy lives in the trigger.

#include "lib/log.hpp"
#include "lib/protocol.hpp"
#include "lib/uds.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* DEFAULT_SOCKET = "/run/focus-event.sock";
constexpr unsigned DEFAULT_MODE = 0666;

void print_usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " --expected-trigger PATH [--socket PATH] [--mode MODE]\n"
        << "  --expected-trigger PATH  Required. Store path of the trusted\n"
        << "                           focus-event-trigger binary. Connections\n"
        << "                           whose /proc/<pid>/exe doesn't resolve to\n"
        << "                           exactly this path are rejected.\n"
        << "  --socket PATH            Unix socket path (default: " << DEFAULT_SOCKET << ")\n"
        << "  --mode MODE              Socket file mode in octal (default: "
        << std::oct << DEFAULT_MODE << std::dec << ")\n"
        << "  --insecure-no-verify     Disable binary verification. For testing\n"
        << "                           only; never use in production.\n";
}

// ---------------------------------------------------------------------------
// Spawn pipeline
//
// Each spawn:
//   1. fork() (single fork, not double-fork — we want to know how it ended).
//   2. Child sets up a stderr pipe, redirects stdin/stdout to /dev/null, and
//      execvp()s the argv. If execvp fails, the child writes the errno string
//      to the stderr pipe and exits 127.
//   3. Parent returns immediately. The pid + argv[0] + stderr-pipe-fd are
//      stashed in a registry.
//   4. A dedicated reaper thread waitpid()s any child, looks up the registry,
//      drains the stderr pipe, and logs the outcome.
//
// This means failures (wrong binary, can't connect to keyd socket, parse
// error in the command, non-zero exit) all show up in the journal as
// `executor: \`<cmd>\` (pid N) failed: ...` plus the captured stderr.

struct PendingChild {
    std::string argv0;          // for log messages
    std::string full_cmd;       // "<argv0> <arg1> <arg2> ..." for the log line
    int stderr_fd = -1;         // read end; closed by reaper after drain
};

std::mutex g_pending_mu;
std::unordered_map<pid_t, PendingChild> g_pending;

// Format argv as a shell-ish single-line representation, for log lines.
std::string format_cmd(const std::vector<std::string>& argv) {
    std::string out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) out += ' ';
        bool needs_quote = argv[i].find_first_of(" \t'\"") != std::string::npos;
        if (needs_quote) {
            out += '\'';
            for (char c : argv[i]) {
                if (c == '\'') out += "'\\''";
                else out += c;
            }
            out += '\'';
        } else {
            out += argv[i];
        }
    }
    return out;
}

// Read everything available on fd (non-blocking after we set it). Returns
// up to ~8 KiB; anything beyond is dropped to bound our memory.
std::string drain_stderr(int fd) {
    if (fd < 0) return {};
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    std::string out;
    char buf[4096];
    while (out.size() < 8192) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            break;  // EAGAIN or any other error: stop
        }
    }
    return out;
}

void log_outcome(pid_t pid, const PendingChild& pc, int status, const std::string& child_output) {
    bool failed = !WIFEXITED(status) || WEXITSTATUS(status) != 0;

    // Outcome line. We always compute it (cheap) and route to ERROR on
    // failure or INFO on success — so default (error) logging shows only
    // the failed spawns, while bumping the level reveals successful ones.
    std::ostringstream status_line;
    status_line << "executor: `" << pc.full_cmd << "` (pid " << pid << ") ";
    if (WIFEXITED(status)) {
        status_line << "exited " << WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        status_line << "killed by signal " << WTERMSIG(status)
                    << " (" << ::strsignal(WTERMSIG(status)) << ")";
    } else {
        status_line << "unknown status " << status;
    }
    status_line << "\n";
    if (failed) LOG_ERROR(status_line.str());
    else        LOG_INFO(status_line.str());

    // Print captured child output (stdout + stderr combined). Indent any
    // embedded newlines so multi-line output stays a clean block in the
    // journal. Routed the same way as the status line: ERROR on failure,
    // INFO on success.
    if (!child_output.empty()) {
        std::string trimmed = child_output;
        while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) {
            trimmed.pop_back();
        }
        if (!trimmed.empty()) {
            std::ostringstream block;
            block << "executor: output: ";
            for (char c : trimmed) {
                if (c == '\n') block << "\nexecutor: output: ";
                else block << c;
            }
            block << "\n";
            if (failed) LOG_ERROR(block.str());
            else        LOG_INFO(block.str());
        }
    }
}

void reaper_loop(std::atomic<bool>& shutdown) {
    while (!shutdown.load()) {
        int status = 0;
        pid_t pid = ::waitpid(-1, &status, 0);
        if (pid < 0) {
            if (errno == EINTR) continue;
            if (errno == ECHILD) {
                // No children outstanding; sleep briefly to avoid spinning.
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            LOG_ERROR("executor: waitpid error: " << std::strerror(errno) << "\n");
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        PendingChild pc;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_pending_mu);
            auto it = g_pending.find(pid);
            if (it != g_pending.end()) {
                pc = std::move(it->second);
                g_pending.erase(it);
                found = true;
            }
        }
        if (!found) {
            // Unknown child — shouldn't happen for our spawns, but don't crash.
            continue;
        }

        std::string err = drain_stderr(pc.stderr_fd);
        if (pc.stderr_fd >= 0) ::close(pc.stderr_fd);

        log_outcome(pid, pc, status, err);
    }
}

// Fork + execvp argv, capturing stderr via a pipe. Returns immediately; the
// reaper thread will log the exit status. Best-effort: if pipe/fork fail we
// log and return.
//
// `env` is the (key, value) list to run the child with. We replace the
// child's environment entirely (clearenv + setenv loop) so the child sees
// exactly what the trigger sent us — typically the user-session env, not the
// system service's minimal env.
void spawn_tracked(const std::vector<std::string>& argv,
                   const std::vector<std::pair<std::string, std::string>>& env) {
    if (argv.empty()) return;

    int errpipe[2];
    if (::pipe2(errpipe, O_CLOEXEC) < 0) {
        LOG_ERROR("executor: pipe() failed for `" << argv[0]
                  << "`: " << std::strerror(errno) << "\n");
        return;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(errpipe[0]);
        ::close(errpipe[1]);
        LOG_ERROR("executor: fork() failed for `" << argv[0]
                  << "`: " << std::strerror(errno) << "\n");
        return;
    }

    if (pid == 0) {
        // Child
        ::close(errpipe[0]);
        // Capture BOTH stdout and stderr. Many tools (e.g. keyd) write their
        // actual diagnostic — "Success" or the error message — to stdout,
        // not stderr. Redirecting both to the same pipe means we see what
        // the child actually said.
        if (::dup2(errpipe[1], STDOUT_FILENO) < 0) ::_exit(126);
        if (::dup2(errpipe[1], STDERR_FILENO) < 0) ::_exit(126);
        ::close(errpipe[1]);

        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::close(devnull);
        }

        // Reset signal handlers to defaults; clear any blocked signals.
        ::signal(SIGPIPE, SIG_DFL);
        sigset_t empty;
        sigemptyset(&empty);
        ::sigprocmask(SIG_SETMASK, &empty, nullptr);

        // Replace our environment with the trigger's snapshot. Clear first
        // so we don't leak anything inherited from the systemd unit (which
        // could include secrets the child shouldn't see). Strip LD_* as a
        // defense-in-depth against accidental privilege escalation via
        // shared-library injection into a root-running child.
        ::clearenv();
        for (const auto& [k, v] : env) {
            if (k == "LD_PRELOAD" || k == "LD_LIBRARY_PATH" ||
                k.rfind("LD_", 0) == 0) {
                continue;
            }
            ::setenv(k.c_str(), v.c_str(), 1);
        }

        std::vector<std::string> storage = argv;
        std::vector<char*> ptrs;
        ptrs.reserve(storage.size() + 1);
        for (auto& a : storage) ptrs.push_back(a.data());
        ptrs.push_back(nullptr);
        ::execvp(ptrs[0], ptrs.data());

        // execvp failed — write the reason to stderr (which is the pipe) and exit.
        std::string err = std::string("execvp ") + argv[0] + ": " + std::strerror(errno) + "\n";
        ssize_t w = ::write(STDERR_FILENO, err.data(), err.size());
        (void)w;
        ::_exit(127);
    }

    // Parent
    ::close(errpipe[1]);
    PendingChild pc;
    pc.argv0 = argv[0];
    pc.full_cmd = format_cmd(argv);
    pc.stderr_fd = errpipe[0];
    {
        std::lock_guard<std::mutex> lock(g_pending_mu);
        g_pending[pid] = std::move(pc);
    }
}

// Parse an octal mode like "0666" or "666". Throws on garbage.
unsigned parse_mode(const std::string& s) {
    errno = 0;
    char* end = nullptr;
    unsigned long m = std::strtoul(s.c_str(), &end, 8);
    if (errno != 0 || end == s.c_str() || *end != '\0' || m > 0777) {
        throw std::runtime_error("invalid mode: " + s);
    }
    return static_cast<unsigned>(m);
}

// Resolve /proc/<pid>/exe to its target. Returns empty string on failure
// (process exited, no permission, /proc not mounted, etc.).
std::string resolve_exe_path(pid_t pid) {
    std::string link = "/proc/" + std::to_string(pid) + "/exe";
    char buf[PATH_MAX];
    ssize_t n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
    if (n < 0) return {};
    buf[n] = '\0';
    return std::string(buf);
}

struct AuthConfig {
    std::string expected_trigger;   // path the peer's /proc/<pid>/exe must match
    bool insecure_no_verify = false;
};

// Verify the connecting peer is running the trusted trigger binary.
// Returns true iff /proc/<pid>/exe resolves to expected_trigger.
bool verify_peer(const AuthConfig& auth, pid_t peer_pid, uid_t peer_uid) {
    if (auth.insecure_no_verify) {
        LOG_ERROR("executor: WARNING binary verification disabled, accepting\n");
        return true;
    }
    if (auth.expected_trigger.empty()) {
        LOG_ERROR("executor: no --expected-trigger configured, rejecting\n");
        return false;
    }
    std::string exe = resolve_exe_path(peer_pid);
    if (exe.empty()) {
        LOG_ERROR("executor: cannot resolve /proc/" << peer_pid << "/exe, rejecting\n");
        return false;
    }
    if (exe != auth.expected_trigger) {
        LOG_ERROR("executor: rejecting pid " << peer_pid << " uid " << peer_uid
                  << ": binary " << exe << " != expected " << auth.expected_trigger << "\n");
        return false;
    }
    return true;
}

void handle_client(int cfd, const AuthConfig& auth) {
    auto creds = uds::peer_creds(cfd);
    if (!creds) {
        LOG_ERROR("executor: peer credentials unavailable, dropping\n");
        return;
    }
    if (!verify_peer(auth, creds->pid, creds->uid)) {
        return;
    }
    LOG_DEBUG("executor: trusted trigger pid=" << creds->pid
              << " uid=" << creds->uid << " connected\n");

    // Per-connection environment snapshot. Empty until the trigger sends
    // a TAG_ENV frame; spawns issued before that fall back to the executor's
    // own (systemd-minimal) env.
    std::vector<std::pair<std::string, std::string>> client_env;

    std::vector<std::uint8_t> payload;
    while (uds::read_frame(cfd, payload)) {
        if (payload.empty()) continue;
        std::uint8_t tag = payload[0];
        const std::uint8_t* body = payload.data() + 1;
        std::size_t body_len = payload.size() - 1;

        if (tag == protocol::TAG_SPAWN) {
            std::vector<std::string> argv;
            try {
                argv = protocol::decode_spawn_payload(body, body_len);
            } catch (const std::exception& e) {
                LOG_ERROR("executor: malformed spawn frame from pid "
                          << creds->pid << ": " << e.what() << "\n");
                return;
            }
            if (argv.empty()) {
                LOG_ERROR("executor: empty spawn argv, ignoring\n");
                continue;
            }
            std::string cmd = format_cmd(argv);
            LOG_INFO("executor: spawn (from pid=" << creds->pid << "): " << cmd << "\n");
            spawn_tracked(argv, client_env);
        } else if (tag == protocol::TAG_HELLO) {
            // Body layout: uint32 label_len, then label bytes. Best-effort log.
            std::string label;
            if (body_len >= 4) {
                std::uint32_t llen = static_cast<std::uint32_t>(body[0])
                                   | (static_cast<std::uint32_t>(body[1]) << 8)
                                   | (static_cast<std::uint32_t>(body[2]) << 16)
                                   | (static_cast<std::uint32_t>(body[3]) << 24);
                if (llen <= body_len - 4) {
                    label.assign(reinterpret_cast<const char*>(body + 4), llen);
                }
            }
            LOG_DEBUG("executor: hello from pid=" << creds->pid
                      << " label='" << label << "'\n");
        } else if (tag == protocol::TAG_ENV) {
            try {
                client_env = protocol::decode_env_payload(body, body_len);
            } catch (const std::exception& e) {
                LOG_ERROR("executor: malformed env frame from pid "
                          << creds->pid << ": " << e.what() << "\n");
                continue;
            }
            // Log a few key vars so the journal shows what spawn will use.
            // Avoid dumping the whole env — it can contain secrets.
            std::size_t shown = 0;
            for (const auto& [k, v] : client_env) {
                if (k == "PATH" || k == "XDG_RUNTIME_DIR" || k == "HOME" ||
                    k == "NIRI_SOCKET" || k == "DBUS_SESSION_BUS_ADDRESS" ||
                    k == "WAYLAND_DISPLAY") {
                    LOG_DEBUG("executor: env " << k << "=" << v << "\n");
                    ++shown;
                }
            }
            LOG_DEBUG("executor: env snapshot from pid=" << creds->pid
                      << ": " << client_env.size() << " entries ("
                      << shown << " shown)\n");
        } else {
            LOG_ERROR("executor: unknown tag " << static_cast<int>(tag)
                      << " from pid=" << creds->pid << ", ignoring\n");
        }
    }
    LOG_DEBUG("executor: client pid=" << creds->pid << " disconnected\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string socket_path = DEFAULT_SOCKET;
    unsigned mode = DEFAULT_MODE;
    AuthConfig auth;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                LOG_ERROR("executor: missing value for " << a << "\n");
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--socket") socket_path = next();
        else if (a == "--mode") mode = parse_mode(next());
        else if (a == "--expected-trigger") auth.expected_trigger = next();
        else if (a == "--insecure-no-verify") auth.insecure_no_verify = true;
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else {
            LOG_ERROR("executor: unknown arg '" << a << "'\n");
            print_usage(argv[0]);
            return 2;
        }
    }

    if (!auth.insecure_no_verify && auth.expected_trigger.empty()) {
        LOG_ERROR("executor: --expected-trigger PATH is required\n"
                  "(use --insecure-no-verify only for local testing)\n");
        return 2;
    }

    int lfd = -1;
    try {
        lfd = uds::listen(socket_path, mode);
    } catch (const std::exception& e) {
        LOG_ERROR("executor: " << e.what() << "\n");
        return 1;
    }

    LOG_INFO("executor: listening on " << socket_path
             << " mode=" << std::oct << mode << std::dec << "\n");
    if (auth.insecure_no_verify) {
        LOG_ERROR("executor: WARNING verification disabled — any local"
                  << " user that can connect can spawn arbitrary commands\n");
    } else {
        LOG_INFO("executor: trusting only trigger binary at "
                 << auth.expected_trigger << "\n");
    }
    // Log PATH so `execvp <binary>` failures (command not found) are
    // diagnosable from the journal alone.
    if (const char* p = std::getenv("PATH")) {
        LOG_DEBUG("executor: PATH=" << p << "\n");
        (void)p;  // silence unused-variable when LOG_LEVEL < DEBUG
    } else {
        LOG_ERROR("executor: PATH is unset — spawn commands by absolute path "
                  << "or set Environment=PATH=... in the systemd unit\n");
    }

    ::signal(SIGPIPE, SIG_IGN);

    // Reaper thread logs exit status + stderr of every spawn. Lives for the
    // lifetime of the daemon; no shutdown synchronization needed since the
    // accept loop below never returns.
    std::atomic<bool> shutdown{false};
    std::thread reaper(reaper_loop, std::ref(shutdown));
    reaper.detach();

    while (true) {
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("executor: accept: " << std::strerror(errno) << "\n");
            continue;
        }
        ::fcntl(cfd, F_SETFD, FD_CLOEXEC);
        handle_client(cfd, auth);
        ::close(cfd);
    }
    return 0;
}
