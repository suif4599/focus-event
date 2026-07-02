// Minimal KDL-like parser supporting the subset we need for config files.
//
// Supported grammar (per node):
//   node := IDENT (value)* (PROP)* ('{' child_node* '}')?
//   value := STRING | RAW_STRING | NUMBER | 'true' | 'false' | 'null'
//   prop := IDENT '=' value
//   STRING := '"' ... '"'   with escape sequences \n \t \\ \" \uXXXX
//   RAW_STRING := 'r"' ... '"'  (no escape processing)
// Comments: // line, /* block */
//
// Each node has: name, positional string values, properties (name -> string),
// and ordered child nodes. Source positions are tracked for error messages.

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kdl {

struct Node {
    std::string name;
    std::vector<std::string> values;          // positional arguments
    std::vector<std::pair<std::string, std::string>> props;  // key=value (insertion order preserved)
    std::vector<Node> children;               // child nodes

    bool has_prop(std::string_view key) const;
    std::optional<std::string> get_prop(std::string_view key) const;
};

struct ParseError : std::runtime_error {
    std::size_t line;
    std::size_t col;
    ParseError(std::size_t l, std::size_t c, std::string msg)
        : std::runtime_error(format(l, c, std::move(msg))), line(l), col(c) {}
    static std::string format(std::size_t l, std::size_t c, std::string msg) {
        return "kdl:" + std::to_string(l) + ":" + std::to_string(c) + ": " + msg;
    }
};

// Parse an entire KDL document. Returns top-level nodes.
std::vector<Node> parse(std::string_view src);

} // namespace kdl
