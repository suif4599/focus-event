#include "niri_socket.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace niri_socket {

namespace {

int connect_blocking(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        throw std::runtime_error("NIRI_SOCKET path too long");
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int err = errno;
        ::close(fd);
        throw std::runtime_error(std::string("connect ") + path + ": " + std::strerror(err));
    }
    return fd;
}

void send_request(int fd, const char* req) {
    std::string line = std::string("\"") + req + "\"\n";
    std::size_t off = 0;
    while (off < line.size()) {
        ssize_t n = ::write(fd, line.data() + off, line.size() - off);
        if (n > 0) {
            off += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error(std::string("write: ") + std::strerror(errno));
        }
    }
}

// Blocking single-line read. Byte-at-a-time on purpose: a chunked read could
// consume bytes past the '\n' (e.g. the first event line after the handshake
// reply) that this interface has no way to hand back.
std::string read_line(int fd) {
    std::string out;
    char c = 0;
    while (true) {
        ssize_t n = ::read(fd, &c, 1);
        if (n > 0) {
            if (c == '\n') return out;
            out.push_back(c);
        } else if (n == 0) {
            throw std::runtime_error("niri closed the socket before replying");
        } else if (errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error(std::string("read: ") + std::strerror(errno));
        }
    }
}

} // namespace

std::string socket_path() {
    const char* path = std::getenv("NIRI_SOCKET");
    if (!path || !*path) {
        throw std::runtime_error("NIRI_SOCKET is not set, are you running this within niri?");
    }
    return path;
}

int connect_event_stream() {
    int fd = connect_blocking(socket_path());
    try {
        send_request(fd, "EventStream");
        std::string reply = read_line(fd);
        niri::expect_handled_reply(reply);
    } catch (...) {
        ::close(fd);
        throw;
    }
    // We never write again; half-close like niri's own client does.
    ::shutdown(fd, SHUT_WR);
    return fd;
}

std::vector<niri::Window> query_windows() {
    int fd = connect_blocking(socket_path());
    std::vector<niri::Window> out;
    try {
        send_request(fd, "Windows");
        out = niri::parse_windows_reply(read_line(fd));
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
    return out;
}

std::optional<niri::Window> query_focused_window() {
    int fd = connect_blocking(socket_path());
    std::optional<niri::Window> out;
    try {
        send_request(fd, "FocusedWindow");
        out = niri::parse_focused_window_reply(read_line(fd));
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
    return out;
}

} // namespace niri_socket
