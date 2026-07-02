#include "uds.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace uds {

namespace {

void fill_addr(sockaddr_un& addr, const std::string& path) {
    if (path.size() >= sizeof(addr.sun_path)) {
        throw std::runtime_error("socket path too long: " + path);
    }
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
}

} // namespace

int listen(const std::string& path, unsigned mode) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

    // Best-effort cleanup of any stale socket file from a previous run.
    ::unlink(path.c_str());

    sockaddr_un addr;
    fill_addr(addr, path);

    // Bind with mode 0777 first, then chmod to the requested mode. umask
    // could interfere between bind and chmod, so we chmod explicitly.
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        ::close(fd);
        throw std::runtime_error(std::string("bind ") + path + ": " + std::strerror(e));
    }
    if (::chmod(path.c_str(), mode) < 0) {
        int e = errno;
        ::close(fd);
        ::unlink(path.c_str());
        throw std::runtime_error(std::string("chmod ") + path + ": " + std::strerror(e));
    }

    if (::listen(fd, 16) < 0) {
        int e = errno;
        ::close(fd);
        ::unlink(path.c_str());
        throw std::runtime_error(std::string("listen: ") + std::strerror(e));
    }
    return fd;
}

int connect(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

    sockaddr_un addr;
    fill_addr(addr, path);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        ::close(fd);
        throw std::runtime_error(std::string("connect ") + path + ": " + std::strerror(e));
    }
    return fd;
}

std::optional<PeerCreds> peer_creds(int fd) {
    ucred cred;
    socklen_t len = sizeof(cred);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) {
        return std::nullopt;
    }
    return PeerCreds{cred.pid, cred.uid, cred.gid};
}

bool read_frame(int fd, std::vector<std::uint8_t>& out_payload) {
    // Read 4-byte length header.
    std::uint8_t header[4];
    std::size_t filled = 0;
    while (filled < 4) {
        ssize_t n = ::read(fd, header + filled, 4 - filled);
        if (n > 0) { filled += static_cast<std::size_t>(n); continue; }
        if (n == 0) return false; // EOF
        if (errno == EINTR) continue;
        return false;
    }

    std::uint32_t len = static_cast<std::uint32_t>(header[0])
                      | (static_cast<std::uint32_t>(header[1]) << 8)
                      | (static_cast<std::uint32_t>(header[2]) << 16)
                      | (static_cast<std::uint32_t>(header[3]) << 24);

    // 1 MiB cap to prevent a malformed peer from OOMing us.
    if (len > (1u << 20)) return false;

    out_payload.resize(len);
    filled = 0;
    while (filled < len) {
        ssize_t n = ::read(fd, out_payload.data() + filled, len - filled);
        if (n > 0) { filled += static_cast<std::size_t>(n); continue; }
        if (n == 0) return false;
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

bool write_all(int fd, const std::uint8_t* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::write(fd, data + sent, len - sent);
        if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

} // namespace uds
