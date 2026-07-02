#include "kdl.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace kdl {

bool Node::has_prop(std::string_view key) const {
    for (const auto& [k, v] : props) {
        if (k == key) return true;
    }
    return false;
}

std::optional<std::string> Node::get_prop(std::string_view key) const {
    for (const auto& [k, v] : props) {
        if (k == key) return v;
    }
    return std::nullopt;
}

namespace {

class Lexer {
public:
    explicit Lexer(std::string_view src) : src_(src) {}

    struct Token {
        enum Kind {
            IDENT,        // bare word (identifier or unquoted value)
            STRING,       // quoted string (already unescaped)
            NUMBER,       // numeric literal (kept as text)
            BOOL,         // true/false
            NULL_LIT,     // null
            LBRACE,       // {
            RBRACE,       // }
            EQUALS,       // =
            EOF_T,
        } kind;
        std::string text;
        std::size_t line;
        std::size_t col;
    };

    Token next() {
        skip_ws_and_comments();
        if (at_end()) return {Token::EOF_T, "", line_, col_};

        char c = peek();
        std::size_t start_line = line_, start_col = col_;

        if (c == '{') { advance(); return {Token::LBRACE, "{", start_line, start_col}; }
        if (c == '}') { advance(); return {Token::RBRACE, "}", start_line, start_col}; }
        if (c == '=') { advance(); return {Token::EQUALS, "=", start_line, start_col}; }

        if (c == '"') return read_quoted(false, start_line, start_col);
        if (c == 'r' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '"') {
            advance(); // consume 'r'
            return read_quoted(true, start_line, start_col);
        }
        if (c == '#') {
            // Hash-quoted raw strings: #"..."# (with N hashes). We rarely need it.
            std::size_t hashes = 0;
            while (peek() == '#') { advance(); ++hashes; }
            if (peek() == '"') return read_hash_quoted(hashes, start_line, start_col);
            throw ParseError(line_, col_, "expected '\"' after '#' sequence");
        }

        // Bare word / number / keyword
        std::string buf;
        while (!at_end()) {
            char d = peek();
            if (d == '{' || d == '}' || d == '=' || d == '"' || std::isspace((unsigned char)d)) break;
            // Comments shouldn't be part of an identifier; we already skipped them above.
            buf.push_back(d);
            advance();
        }
        if (buf.empty()) {
            throw ParseError(line_, col_, std::string("unexpected character '") + c + "'");
        }

        Token::Kind k = classify(buf);
        return {k, buf, start_line, start_col};
    }

private:
    std::string_view src_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t col_ = 1;

    char peek() const { return src_[pos_]; }
    bool at_end() const { return pos_ >= src_.size(); }
    void advance() {
        char c = src_[pos_++];
        if (c == '\n') { ++line_; col_ = 1; } else { ++col_; }
    }

    void skip_ws_and_comments() {
        while (!at_end()) {
            char c = peek();
            if (std::isspace((unsigned char)c)) {
                advance();
            } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                while (!at_end() && peek() != '\n') advance();
            } else if (c == '/' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '*') {
                advance(); advance();
                while (!at_end() && !(peek() == '*' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/')) advance();
                if (at_end()) throw ParseError(line_, col_, "unterminated block comment");
                advance(); advance();
            } else {
                break;
            }
        }
    }

    static bool is_ident_start(char c) {
        return std::isalpha((unsigned char)c) || c == '_' || c == '-' || c == '.';
    }

    Token read_quoted(bool raw, std::size_t sl, std::size_t sc) {
        advance(); // consume opening "
        std::string buf;
        while (!at_end() && peek() != '"') {
            char c = peek();
            if (!raw && c == '\\') {
                advance();
                if (at_end()) throw ParseError(line_, col_, "unterminated escape");
                char e = peek();
                switch (e) {
                    case 'n': buf.push_back('\n'); advance(); break;
                    case 't': buf.push_back('\t'); advance(); break;
                    case 'r': buf.push_back('\r'); advance(); break;
                    case '\\': buf.push_back('\\'); advance(); break;
                    case '"': buf.push_back('"'); advance(); break;
                    case '0': buf.push_back('\0'); advance(); break;
                    default:
                        if (e == 'u') {
                            advance(); // consume 'u'
                            std::string hex;
                            for (int i = 0; i < 4 && !at_end(); ++i) {
                                hex.push_back(peek()); advance();
                            }
                            // Encode back as UTF-8
                            unsigned cp = std::stoul(hex, nullptr, 16);
                            encode_utf8(cp, buf);
                        } else {
                            buf.push_back('\\');
                            buf.push_back(e);
                            advance();
                        }
                }
            } else {
                buf.push_back(c);
                advance();
            }
        }
        if (at_end()) throw ParseError(line_, col_, "unterminated string literal");
        advance(); // consume closing "
        return {Token::STRING, buf, sl, sc};
    }

    Token read_hash_quoted(std::size_t hashes, std::size_t sl, std::size_t sc) {
        advance(); // consume opening "
        std::string buf;
        while (true) {
            if (at_end()) throw ParseError(line_, col_, "unterminated raw string");
            if (peek() == '"') {
                bool ok = true;
                for (std::size_t i = 0; i < hashes; ++i) {
                    if (pos_ + 1 + i >= src_.size() || src_[pos_ + 1 + i] != '#') { ok = false; break; }
                }
                if (ok) {
                    advance(); // closing "
                    for (std::size_t i = 0; i < hashes; ++i) advance(); // closing hashes
                    return {Token::STRING, buf, sl, sc};
                }
            }
            buf.push_back(peek());
            advance();
        }
    }

    static void encode_utf8(unsigned cp, std::string& out) {
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    static Token::Kind classify(const std::string& s) {
        if (s == "true" || s == "false") return Token::BOOL;
        if (s == "null") return Token::NULL_LIT;
        // Numbers: detect first char
        char c = s[0];
        if (c == '-' || c == '+' || std::isdigit((unsigned char)c)) {
            // Could be a number; we don't strictly validate but treat as NUMBER.
            return Token::NUMBER;
        }
        return Token::IDENT;
    }
};

// Parse a single node starting at the given token stream position.
// `lex` is the lexer; we read tokens one at a time.
class Parser {
public:
    explicit Parser(std::string_view src) : lex_(src) {
        // Pre-fetch nothing; we pull on demand.
    }

    std::vector<Node> parse_document() {
        std::vector<Node> nodes;
        Token t = next();
        while (t.kind != Token::EOF_T) {
            push_back(t);
            nodes.push_back(parse_node(next()));
            t = next();
        }
        return nodes;
    }

private:
    using Token = Lexer::Token;

    Lexer lex_;
    std::vector<Token> pending_;

    void push_back(Token t) { pending_.push_back(std::move(t)); }

    Token next() {
        if (!pending_.empty()) {
            Token t = std::move(pending_.back());
            pending_.pop_back();
            return t;
        }
        return lex_.next();
    }

    Token expect(Lexer::Token::Kind k, const char* what) {
        Token t = next();
        if (t.kind != k) {
            std::string got = (t.kind == Token::EOF_T) ? "end of input" : t.text;
            throw ParseError(t.line, t.col, std::string("expected ") + what + ", got '" + got + "'");
        }
        return t;
    }

    Node parse_node(Token first) {
        // first must be IDENT (the node name)
        if (first.kind != Token::IDENT) {
            throw ParseError(first.line, first.col,
                             "expected node name (identifier), got '" + first.text + "'");
        }
        Node node;
        node.name = first.text;

        // Read positional values and props until '{' or another node or EOF.
        //
        // KDL semantics we adopt here:
        //   - Positional values must be STRING/NUMBER/BOOL/NULL_LIT.
        //   - A bare IDENT is either a property key (if followed by '=') or the
        //     start of the next sibling node (in which case we push it back and
        //     return). It is never treated as a positional value.
        while (true) {
            Token t = next();
            if (t.kind == Token::LBRACE) {
                // Parse children
                Token c = next();
                while (c.kind != Token::RBRACE) {
                    if (c.kind == Token::EOF_T) {
                        throw ParseError(c.line, c.col, "expected '}' before end of input");
                    }
                    if (c.kind != Token::IDENT) {
                        throw ParseError(c.line, c.col,
                                         "expected node name as child, got '" + c.text + "'");
                    }
                    push_back(c);
                    node.children.push_back(parse_node(next()));
                    c = next();
                }
                break;
            }
            if (t.kind == Token::EOF_T) {
                break;
            }
            if (t.kind == Token::RBRACE) {
                // End of an enclosing children block — let the parent consume it.
                push_back(std::move(t));
                break;
            }
            if (t.kind == Token::IDENT) {
                // Either prop key, or start of a sibling node.
                Token nxt = next();
                if (nxt.kind == Token::EQUALS) {
                    Token val = next();
                    if (val.kind != Token::STRING && val.kind != Token::NUMBER &&
                        val.kind != Token::BOOL && val.kind != Token::NULL_LIT &&
                        val.kind != Token::IDENT) {
                        throw ParseError(val.line, val.col, "expected value after '='");
                    }
                    node.props.emplace_back(t.text, val.text);
                } else {
                    // IDENT not followed by '=' => start of a sibling node.
                    // Push both back so the caller's loop picks up the sibling.
                    // Stack is LIFO, so push nxt first to make t come out first.
                    push_back(std::move(nxt));
                    push_back(std::move(t));
                    break;
                }
            } else if (t.kind == Token::STRING || t.kind == Token::NUMBER ||
                       t.kind == Token::BOOL || t.kind == Token::NULL_LIT) {
                node.values.push_back(t.text);
            } else {
                // EQUALS at this position is invalid
                throw ParseError(t.line, t.col, "unexpected token '" + t.text + "'");
            }
        }
        return node;
    }
};

} // namespace

std::vector<Node> parse(std::string_view src) {
    Parser p(src);
    return p.parse_document();
}

} // namespace kdl
