#include "schema/expr.hpp"

#include <cctype>
#include <stdexcept>
#include <vector>

namespace gmpc {

namespace {

struct Tok {
    enum class Kind { Int, Ident, Op, End } kind;
    std::string text;
    std::int64_t value = 0;
};

struct ParseFail : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::vector<Tok> lex(const std::string& s) {
    std::vector<Tok> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t j = i;
            int base = 10;
            if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) { base = 16; j += 2; }
            else if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'b' || s[i + 1] == 'B')) { base = 2; j += 2; }
            const std::size_t start = j;
            while (j < s.size() && std::isalnum(static_cast<unsigned char>(s[j]))) ++j;
            const std::string digits = s.substr(start, j - start);
            if (digits.empty()) throw ParseFail("malformed number");
            std::size_t used = 0;
            std::int64_t v = 0;
            try {
                v = static_cast<std::int64_t>(std::stoull(digits, &used, base));
            } catch (...) {
                throw ParseFail("malformed number '" + s.substr(i, j - i) + "'");
            }
            if (used != digits.size()) throw ParseFail("malformed number '" + s.substr(i, j - i) + "'");
            out.push_back({Tok::Kind::Int, s.substr(i, j - i), v});
            i = j;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t j = i;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
            out.push_back({Tok::Kind::Ident, s.substr(i, j - i)});
            i = j;
            continue;
        }
        static const char* two[] = {"||", "&&", "==", "!=", "<=", ">=", "<<", ">>"};
        bool matched = false;
        for (const char* t : two) {
            if (s.compare(i, 2, t) == 0) {
                out.push_back({Tok::Kind::Op, t});
                i += 2;
                matched = true;
                break;
            }
        }
        if (matched) continue;
        if (std::string("+-*/%&|^!~<>().").find(c) != std::string::npos) {
            out.push_back({Tok::Kind::Op, std::string(1, c)});
            ++i;
            continue;
        }
        throw ParseFail(std::string("unexpected character '") + c + "'");
    }
    out.push_back({Tok::Kind::End, ""});
    return out;
}

class Parser {
public:
    Parser(std::vector<Tok> toks, SourceLoc loc) : toks_(std::move(toks)), loc_(std::move(loc)) {}

    ir::ExprPtr parse() {
        auto e = parse_or();
        if (peek().kind != Tok::Kind::End) throw ParseFail("unexpected '" + peek().text + "'");
        return e;
    }

private:
    const Tok& peek() const { return toks_[pos_]; }
    bool is_op(const char* op) const { return peek().kind == Tok::Kind::Op && peek().text == op; }
    Tok take() { return toks_[pos_++]; }

    ir::ExprPtr make_bin(std::string op, ir::ExprPtr l, ir::ExprPtr r) {
        auto e = std::make_shared<ir::Expr>();
        e->kind = ir::Expr::Kind::Binary;
        e->loc = loc_;
        e->op = std::move(op);
        e->lhs = std::move(l);
        e->rhs = std::move(r);
        return e;
    }

    template <class Next>
    ir::ExprPtr left_assoc(std::initializer_list<const char*> ops, Next next) {
        auto l = (this->*next)();
        for (;;) {
            bool found = false;
            for (const char* op : ops) {
                if (is_op(op)) {
                    take();
                    l = make_bin(op, l, (this->*next)());
                    found = true;
                    break;
                }
            }
            if (!found) return l;
        }
    }

    ir::ExprPtr parse_or() { return left_assoc({"||"}, &Parser::parse_and); }
    ir::ExprPtr parse_and() { return left_assoc({"&&"}, &Parser::parse_cmp); }
    ir::ExprPtr parse_cmp() {
        auto l = parse_bitor();
        for (const char* op : {"==", "!=", "<=", ">=", "<", ">"}) {
            if (is_op(op)) {
                take();
                return make_bin(op, l, parse_bitor());
            }
        }
        return l;
    }
    ir::ExprPtr parse_bitor() { return left_assoc({"|"}, &Parser::parse_bitxor); }
    ir::ExprPtr parse_bitxor() { return left_assoc({"^"}, &Parser::parse_bitand); }
    ir::ExprPtr parse_bitand() { return left_assoc({"&"}, &Parser::parse_shift); }
    ir::ExprPtr parse_shift() { return left_assoc({"<<", ">>"}, &Parser::parse_add); }
    ir::ExprPtr parse_add() { return left_assoc({"+", "-"}, &Parser::parse_mul); }
    ir::ExprPtr parse_mul() { return left_assoc({"*", "/", "%"}, &Parser::parse_unary); }

    ir::ExprPtr parse_unary() {
        for (const char* op : {"!", "-", "~"}) {
            if (is_op(op)) {
                take();
                auto e = std::make_shared<ir::Expr>();
                e->kind = ir::Expr::Kind::Unary;
                e->loc = loc_;
                e->op = op;
                e->lhs = parse_unary();
                return e;
            }
        }
        return parse_primary();
    }

    ir::ExprPtr parse_primary() {
        const Tok t = take();
        auto e = std::make_shared<ir::Expr>();
        e->loc = loc_;
        if (t.kind == Tok::Kind::Int) {
            e->kind = ir::Expr::Kind::Int;
            e->value = t.value;
            return e;
        }
        if (t.kind == Tok::Kind::Ident) {
            e->kind = ir::Expr::Kind::Ref;
            e->path.push_back(t.text);
            while (is_op(".")) {
                take();
                const Tok n = take();
                if (n.kind != Tok::Kind::Ident) throw ParseFail("expected identifier after '.'");
                e->path.push_back(n.text);
            }
            return e;
        }
        if (t.kind == Tok::Kind::Op && t.text == "(") {
            auto inner = parse_or();
            if (!is_op(")")) throw ParseFail("expected ')'");
            take();
            return inner;
        }
        throw ParseFail(t.kind == Tok::Kind::End ? "unexpected end of expression" : "unexpected '" + t.text + "'");
    }

    std::vector<Tok> toks_;
    SourceLoc loc_;
    std::size_t pos_ = 0;
};

} // namespace

ir::ExprPtr parse_expr(const std::string& text, const SourceLoc& loc, Diagnostics& diag) {
    try {
        Parser p(lex(text), loc);
        return p.parse();
    } catch (const ParseFail& e) {
        diag.error(loc, "invalid expression '" + text + "': " + e.what());
        return nullptr;
    }
}

std::string expr_to_string(const ir::Expr& e) {
    switch (e.kind) {
        case ir::Expr::Kind::Int: return std::to_string(e.value);
        case ir::Expr::Kind::Ref:
        case ir::Expr::Kind::EnumConst: {
            std::string s;
            for (std::size_t i = 0; i < e.path.size(); ++i) s += (i ? "." : "") + e.path[i];
            return s;
        }
        case ir::Expr::Kind::Unary: return e.op + "(" + expr_to_string(*e.lhs) + ")";
        case ir::Expr::Kind::Binary:
            return "(" + expr_to_string(*e.lhs) + " " + e.op + " " + expr_to_string(*e.rhs) + ")";
    }
    return "?";
}

} // namespace gmpc
