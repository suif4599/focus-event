// Direct client for niri's IPC socket ($NIRI_SOCKET). Line-delimited JSON:
// one request string per line, one {"Ok":...}/{"Err":"..."} reply per line.
// The event stream is a persistent connection: "EventStream" request,
// {"Ok":"Handled"} reply, then events one-JSON-per-line forever.
//
// This is the same protocol `niri msg` itself speaks; talking to the socket
// directly avoids fork+exec per query and any CLI/compositor version skew.

#pragma once

#include "niri.hpp"

#include <optional>
#include <string>
#include <vector>

namespace niri_socket {

// $NIRI_SOCKET value; throws std::runtime_error if unset.
std::string socket_path();

// Blocking connect + "EventStream" handshake. Consumes (and validates) the
// {"Ok":"Handled"} reply line BEFORE returning, so the fd delivers pure
// event lines from the first read. The fd is left blocking; the caller sets
// O_NONBLOCK and owns closing it.
int connect_event_stream();

// One-shot blocking query on a fresh connection: send "Windows", read one
// reply line, close. Throws on transport failure or {"Err":...}.
std::vector<niri::Window> query_windows();

// One-shot blocking query for the focused window; nullopt = none focused.
// Throws on transport failure or {"Err":...}.
std::optional<niri::Window> query_focused_window();

} // namespace niri_socket
