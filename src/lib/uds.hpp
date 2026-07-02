// Unix-domain-socket helpers shared by the executor (server) and the trigger
// (client). Both sides go through here so the framing logic lives in one place.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace uds {

struct PeerCreds {
    pid_t pid;
    uid_t uid;
    gid_t gid;
};

// Create, bind, listen on a UDS at the given path. Removes any stale socket
// file at that path first. Returns the listening fd, or throws on error.
//
// mode: filesystem mode applied to the socket inode (chmod after bind). Use
// 0660 to restrict access to owner+group, 0666 to allow any local user.
int listen(const std::string& path, unsigned mode);

// Connect to a UDS at the given path. Returns the connected fd or throws.
int connect(const std::string& path);

// Read SO_PEERCRED from a connected socket. Returns nullopt if the kernel
// doesn't support it (very old Linux) — caller may choose to reject.
std::optional<PeerCreds> peer_creds(int fd);

// Read a complete length-prefixed frame from fd. Blocks until the full frame
// is available. Returns false on EOF/error, true on success (writes the
// payload — bytes AFTER the 4-byte length header — into *out_payload).
bool read_frame(int fd, std::vector<std::uint8_t>& out_payload);

// Write all bytes; returns false on EPIPE/EIO. Handles short writes and EINTR.
bool write_all(int fd, const std::uint8_t* data, std::size_t len);

} // namespace uds
