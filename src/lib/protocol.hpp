// Wire protocol between focus-event-trigger and focus-event-executor.
//
// The trigger is the long-lived process: it watches the niri event stream and
// holds the rule table. The executor is a privileged, system-level service
// whose only job is to spawn commands on demand. They communicate over an
// anonymous Unix domain socket; the executor verifies each connecting peer
// with SO_PEERCRED before accepting commands.
//
// Message framing: every message is length-prefixed.
//
//   uint32_t payload_len   (little-endian, not including the 4-byte header)
//   uint8_t  payload[payload_len]
//
// Two message types live in the payload, distinguished by a leading tag byte:
//
//   's'  Spawn command. Followed by a uint32 argc and argc length-prefixed
//        argv strings (each: uint32 strlen, then the bytes — NO trailing NUL).
//
//        payload layout (after the 's' tag):
//            uint32 argc
//            for each arg: uint32 len  +  len bytes
//
//   'h'  Hello / identification (optional, for future use). The trigger sends
//        this on connect with its pid/uid so the executor's logs are nicer.
//        Currently no body.

#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace protocol {

constexpr std::uint8_t TAG_SPAWN = 's';
constexpr std::uint8_t TAG_HELLO = 'h';
constexpr std::uint8_t TAG_ENV = 'e';

// Append a little-endian uint32 to buf.
inline void put_u32(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        buf.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

// Read a little-endian uint32 from p (no bounds check; caller ensures >= 4 bytes).
inline std::uint32_t get_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0])
         | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16)
         | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Serialize a spawn command (tag + argc + argv) into a length-prefixed frame.
// Returns the full frame ready to write(): [uint32 total_len][payload bytes].
inline std::vector<std::uint8_t> encode_spawn(const std::vector<std::string>& argv) {
    std::vector<std::uint8_t> payload;
    payload.push_back(TAG_SPAWN);
    put_u32(payload, static_cast<std::uint32_t>(argv.size()));
    for (const auto& a : argv) {
        put_u32(payload, static_cast<std::uint32_t>(a.size()));
        payload.insert(payload.end(), a.begin(), a.end());
    }

    std::vector<std::uint8_t> frame;
    put_u32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

// Serialize a hello message with a caller-provided label (e.g. trigger pid).
inline std::vector<std::uint8_t> encode_hello(const std::string& label) {
    std::vector<std::uint8_t> payload;
    payload.push_back(TAG_HELLO);
    put_u32(payload, static_cast<std::uint32_t>(label.size()));
    payload.insert(payload.end(), label.begin(), label.end());

    std::vector<std::uint8_t> frame;
    put_u32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

// Serialize an env snapshot. Payload layout (after tag):
//   Nuls-separated "KEY=VALUE" entries, terminator = empty entry (two
//   consecutive nuls). We don't length-prefix each entry because nul is
//   illegal inside both keys and values on Unix env.
//
//   payload = entry \0 entry \0 ... \0
//   (a final extra \0 marks end-of-list)
inline std::vector<std::uint8_t> encode_env(
    const std::vector<std::pair<std::string, std::string>>& env) {
    std::string body;
    for (const auto& [k, v] : env) {
        body += k;
        body += '=';
        body += v;
        body += '\0';
    }
    body += '\0'; // terminator

    std::vector<std::uint8_t> payload;
    payload.push_back(TAG_ENV);
    payload.insert(payload.end(), body.begin(), body.end());

    std::vector<std::uint8_t> frame;
    put_u32(frame, static_cast<std::uint32_t>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

// Parse the env payload (bytes AFTER the tag) into (key, value) pairs.
inline std::vector<std::pair<std::string, std::string>>
decode_env_payload(const std::uint8_t* p, std::size_t n) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t i = 0;
    while (i < n) {
        std::size_t start = i;
        while (i < n && p[i] != 0) ++i;
        if (i == start) break; // empty entry = terminator
        std::string entry(reinterpret_cast<const char*>(p + start), i - start);
        ++i; // consume nul
        auto eq = entry.find('=');
        if (eq != std::string::npos) {
            out.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
        }
    }
    return out;
}

// Decode the payload of a SPAWN message (i.e. the bytes AFTER the tag byte).
// Throws std::runtime_error on truncation / oversize.
inline std::vector<std::string> decode_spawn_payload(const std::uint8_t* p, std::size_t n) {
    if (n < 4) throw std::runtime_error("spawn payload too short for argc");
    std::uint32_t argc = get_u32(p);
    p += 4; n -= 4;

    // Cap argc to something sane so a malformed frame can't OOM us.
    if (argc > 4096) throw std::runtime_error("spawn argc implausibly large");

    std::vector<std::string> out;
    out.reserve(argc);
    for (std::uint32_t i = 0; i < argc; ++i) {
        if (n < 4) throw std::runtime_error("spawn arg length truncated");
        std::uint32_t len = get_u32(p);
        p += 4; n -= 4;
        if (len > n) throw std::runtime_error("spawn arg body overruns payload");
        // Cap individual arg length too.
        if (len > 1024 * 1024) throw std::runtime_error("spawn arg implausibly long");
        out.emplace_back(reinterpret_cast<const char*>(p), len);
        p += len; n -= len;
    }
    return out;
}

} // namespace protocol
