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

#include "lib/protocol.hpp"
#include "lib/uds.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
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

// Double-fork + setsid + execvp. Detached so we don't reap.
void spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return;

    pid_t pid = ::fork();
    if (pid < 0) return;
    if (pid > 0) {
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
        return;
    }

    if (::setsid() < 0) ::_exit(127);

    pid_t grand = ::fork();
    if (grand < 0) ::_exit(127);
    if (grand > 0) ::_exit(0);

    int devnull_in = ::open("/dev/null", O_RDONLY);
    int devnull_out = ::open("/dev/null", O_WRONLY);
    if (devnull_in >= 0) { ::dup2(devnull_in, STDIN_FILENO); ::close(devnull_in); }
    if (devnull_out >= 0) {
        ::dup2(devnull_out, STDOUT_FILENO);
        ::dup2(devnull_out, STDERR_FILENO);
        ::close(devnull_out);
    }

    std::vector<std::string> storage = argv;
    std::vector<char*> ptrs;
    ptrs.reserve(storage.size() + 1);
    for (auto& a : storage) ptrs.push_back(a.data());
    ptrs.push_back(nullptr);
    ::execvp(ptrs[0], ptrs.data());
    ::_exit(127);
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
        std::cerr << "executor: WARNING binary verification disabled, accepting\n";
        return true;
    }
    if (auth.expected_trigger.empty()) {
        std::cerr << "executor: no --expected-trigger configured, rejecting\n";
        return false;
    }
    std::string exe = resolve_exe_path(peer_pid);
    if (exe.empty()) {
        std::cerr << "executor: cannot resolve /proc/" << peer_pid << "/exe, rejecting\n";
        return false;
    }
    if (exe != auth.expected_trigger) {
        std::cerr << "executor: rejecting pid " << peer_pid << " uid " << peer_uid
                  << ": binary " << exe << " != expected " << auth.expected_trigger << "\n";
        return false;
    }
    return true;
}

void handle_client(int cfd, const AuthConfig& auth) {
    auto creds = uds::peer_creds(cfd);
    if (!creds) {
        std::cerr << "executor: peer credentials unavailable, dropping\n";
        return;
    }
    if (!verify_peer(auth, creds->pid, creds->uid)) {
        return;
    }
    std::cerr << "executor: trusted trigger pid=" << creds->pid
              << " uid=" << creds->uid << " connected\n";

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
                std::cerr << "executor: malformed spawn frame from pid "
                          << creds->pid << ": " << e.what() << "\n";
                return;
            }
            if (argv.empty()) {
                std::cerr << "executor: empty spawn argv, ignoring\n";
                continue;
            }
            std::string cmd = argv[0];
            for (std::size_t i = 1; i < argv.size(); ++i) {
                cmd += " ";
                cmd += argv[i];
            }
            std::cerr << "executor: spawn (pid=" << creds->pid << "): " << cmd << "\n";
            spawn_detached(argv);
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
            std::cerr << "executor: hello from pid=" << creds->pid
                      << " label='" << label << "'\n";
        } else {
            std::cerr << "executor: unknown tag " << static_cast<int>(tag)
                      << " from pid=" << creds->pid << ", ignoring\n";
        }
    }
    std::cerr << "executor: client pid=" << creds->pid << " disconnected\n";
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
                std::cerr << "executor: missing value for " << a << "\n";
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
            std::cerr << "executor: unknown arg '" << a << "'\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    if (!auth.insecure_no_verify && auth.expected_trigger.empty()) {
        std::cerr << "executor: --expected-trigger PATH is required\n";
        std::cerr << "(use --insecure-no-verify only for local testing)\n";
        return 2;
    }

    int lfd = -1;
    try {
        lfd = uds::listen(socket_path, mode);
    } catch (const std::exception& e) {
        std::cerr << "executor: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "executor: listening on " << socket_path
              << " mode=" << std::oct << mode << std::dec << "\n";
    if (auth.insecure_no_verify) {
        std::cerr << "executor: WARNING verification disabled — any local"
                  << " user that can connect can spawn arbitrary commands\n";
    } else {
        std::cerr << "executor: trusting only trigger binary at "
                  << auth.expected_trigger << "\n";
    }

    ::signal(SIGPIPE, SIG_IGN);

    while (true) {
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            std::cerr << "executor: accept: " << std::strerror(errno) << "\n";
            continue;
        }
        ::fcntl(cfd, F_SETFD, FD_CLOEXEC);
        handle_client(cfd, auth);
        ::close(cfd);
    }
    return 0;
}
