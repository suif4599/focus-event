// focus-event-executor: privileged system service that spawns commands on
// behalf of focus-event-trigger.
//
// Listens on a Unix domain socket (default /run/focus-event.sock). For each
// connecting client:
//   1. Reads SO_PEERCRED. If a uid allowlist was configured via --allow-uid,
//      rejects clients whose uid isn't in the list.
//   2. Reads length-prefixed frames; parses SPAWN messages; fork+setsid+execs
//      the argv. The child is detached so it outlives both trigger and
//      executor (no zombie reaping needed because we double-fork).
//
// Hello messages are accepted but currently only logged.
//
// The daemon is intentionally stateless: it does not load any config and does
// not know about windows or rules. All policy lives in the trigger.

#include "lib/protocol.hpp"
#include "lib/uds.hpp"

#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* DEFAULT_SOCKET = "/run/focus-event.sock";
constexpr unsigned DEFAULT_MODE = 0660;

void print_usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [--socket PATH] [--mode MODE] [--allow-uid N | --allow-user NAME]\n"
        << "  --socket PATH   Unix socket path (default: " << DEFAULT_SOCKET << ")\n"
        << "  --mode MODE     Socket file mode in octal (default: " << std::oct << DEFAULT_MODE << std::dec << ")\n"
        << "  --allow-uid N   Allow connections from this numeric uid (repeatable).\n"
        << "  --allow-user NAME  Allow connections from this username (repeatable).\n"
        << "                  Resolved via getpwnam at runtime, so lazy-uid systems\n"
        << "                  (NixOS) work even if the uid wasn't assigned at build time.\n"
        << "                  If no --allow-* is given, any local uid is allowed\n"
        << "                  (audit-logged via SO_PEERCRED only).\n";
}

// Resolve a username to a uid via getpwnam. Returns -1 if not found.
uid_t resolve_user(const std::string& name) {
    struct passwd* pw = ::getpwnam(name.c_str());
    if (!pw) return static_cast<uid_t>(-1);
    return pw->pw_uid;
}

// Build a snapshot of all allowed uids by resolving every --allow-user name.
// Failed resolutions are silently skipped (the user may not exist yet) — the
// connection attempt will simply not match those entries until they do.
std::set<uid_t> materialize_allowed(const std::set<std::string>& user_names,
                                    const std::set<uid_t>& explicit_uids) {
    std::set<uid_t> out = explicit_uids;
    for (const auto& name : user_names) {
        uid_t u = resolve_user(name);
        if (u != static_cast<uid_t>(-1)) out.insert(u);
    }
    return out;
}

// Double-fork + setsid + execvp. Detached so we don't reap.
void spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return;

    pid_t pid = ::fork();
    if (pid < 0) return;
    if (pid > 0) {
        // Parent: reap the immediate child quickly. The grandchild will be
        // reparented to PID 1.
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
        return;
    }

    // First child: become its own session, then fork again.
    if (::setsid() < 0) ::_exit(127);

    pid_t grand = ::fork();
    if (grand < 0) ::_exit(127);
    if (grand > 0) ::_exit(0);

    // Grandchild: detach stdio and exec.
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

// Parse an octal mode like "0660" or "660". Throws on garbage.
unsigned parse_mode(const std::string& s) {
    errno = 0;
    char* end = nullptr;
    unsigned long m = std::strtoul(s.c_str(), &end, 8);
    if (errno != 0 || end == s.c_str() || *end != '\0' || m > 0777) {
        throw std::runtime_error("invalid mode: " + s);
    }
    return static_cast<unsigned>(m);
}

// Settings that persist for the lifetime of the daemon. The allowed-users are
// re-resolved per connection so that uids assigned after the daemon started
// still work.
struct AllowList {
    std::set<uid_t> explicit_uids;
    std::set<std::string> user_names;
    bool empty() const { return explicit_uids.empty() && user_names.empty(); }
};

// Returns true if the peer's uid is allowed. If the allowlist is empty, any
// uid is allowed (audit-logged only). Usernames are re-resolved on each call
// so newly-created users get picked up without a daemon restart.
bool is_allowed(const AllowList& list, uid_t peer_uid) {
    if (list.empty()) return true;
    if (list.explicit_uids.count(peer_uid)) return true;
    for (const auto& name : list.user_names) {
        uid_t u = resolve_user(name);
        if (u != static_cast<uid_t>(-1) && u == peer_uid) return true;
    }
    return false;
}

void handle_client(int cfd, const AllowList& allow) {
    auto creds = uds::peer_creds(cfd);
    if (!creds) {
        std::cerr << "executor: peer credentials unavailable, dropping\n";
        return;
    }
    if (!is_allowed(allow, creds->uid)) {
        std::cerr << "executor: rejecting uid " << creds->uid
                  << " pid " << creds->pid << " (not in allowlist)\n";
        return;
    }
    std::cerr << "executor: client uid=" << creds->uid
              << " pid=" << creds->pid << " connected\n";

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
                std::cerr << "executor: malformed spawn frame from uid "
                          << creds->uid << ": " << e.what() << "\n";
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
            std::cerr << "executor: spawn (uid=" << creds->uid << "): "
                      << cmd << "\n";
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
            std::cerr << "executor: hello from uid=" << creds->uid
                      << " pid=" << creds->pid << " label='" << label << "'\n";
        } else {
            std::cerr << "executor: unknown tag " << static_cast<int>(tag)
                      << " from uid=" << creds->uid << ", ignoring\n";
        }
    }
    std::cerr << "executor: client uid=" << creds->uid << " disconnected\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string socket_path = DEFAULT_SOCKET;
    unsigned mode = DEFAULT_MODE;
    AllowList allow;

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
        else if (a == "--allow-uid") {
            errno = 0;
            char* end = nullptr;
            unsigned long u = std::strtoul(next().c_str(), &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0') {
                std::cerr << "executor: bad --allow-uid value\n";
                return 2;
            }
            allow.explicit_uids.insert(static_cast<uid_t>(u));
        }
        else if (a == "--allow-user") {
            allow.user_names.insert(next());
        }
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else {
            std::cerr << "executor: unknown arg '" << a << "'\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    int lfd = -1;
    try {
        lfd = uds::listen(socket_path, mode);
    } catch (const std::exception& e) {
        std::cerr << "executor: " << e.what() << "\n";
        return 1;
    }

    // Pre-resolve usernames once for the startup log; the per-connection check
    // re-resolves so users created after startup also work.
    auto initial = materialize_allowed(allow.user_names, allow.explicit_uids);
    std::cerr << "executor: listening on " << socket_path
              << " mode=" << std::oct << mode << std::dec
              << " allowlist=" << initial.size() << " uid(s) ("
              << allow.explicit_uids.size() << " explicit + "
              << allow.user_names.size() << " by-name)"
              << (allow.empty() ? " [no restriction]" : "") << "\n";

    // SIGPIPE: writing to a closed socket must not kill us.
    ::signal(SIGPIPE, SIG_IGN);

    while (true) {
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            std::cerr << "executor: accept: " << std::strerror(errno) << "\n";
            continue;
        }
        ::fcntl(cfd, F_SETFD, FD_CLOEXEC);
        handle_client(cfd, allow);
        ::close(cfd);
    }
    return 0;
}
