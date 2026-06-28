// strata/sql.cpp — a tiny SQL-ish parser mapping onto the Executor (README stretch goal).
#include "strata/sql.hpp"

#include <cctype>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace strata {
namespace {

[[noreturn]] void fail(const std::string& msg) { throw std::runtime_error("strata SQL: " + msg); }

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper((unsigned char)c));
    return s;
}

// ---- tokenizer --------------------------------------------------------------------
struct Token {
    enum Kind { Ident, Num, Str, Op, Punct, End } kind = End;
    std::string text;
};

std::vector<Token> tokenize(const std::string& s) {
    std::vector<Token> out;
    std::size_t i = 0, n = s.size();
    auto isident = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '.'; };
    while (i < n) {
        char c = s[i];
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        if (c == '\'' || c == '"') {  // quoted string literal
            char q = c; std::string v; ++i;
            while (i < n && s[i] != q) v += s[i++];
            if (i >= n) fail("unterminated string literal");
            ++i;  // closing quote
            out.push_back({Token::Str, v});
        } else if (std::isdigit((unsigned char)c)) {
            std::size_t j = i;
            while (j < n && (std::isdigit((unsigned char)s[j]) || s[j] == '.' || s[j] == 'e' ||
                             s[j] == 'E' || ((s[j] == '+' || s[j] == '-') && (s[j-1]=='e'||s[j-1]=='E'))))
                ++j;
            out.push_back({Token::Num, s.substr(i, j - i)});
            i = j;
        } else if (std::isalpha((unsigned char)c) || c == '_') {
            std::size_t j = i;
            while (j < n && isident(s[j])) ++j;
            out.push_back({Token::Ident, s.substr(i, j - i)});
            i = j;
        } else if (c == '<' || c == '>' || c == '=' || c == '!') {  // (multi-char) operators
            std::string op(1, c);
            if (i + 1 < n && (s[i + 1] == '=' || (c == '<' && s[i + 1] == '>'))) { op += s[i + 1]; ++i; }
            out.push_back({Token::Op, op});
            ++i;
        } else if (c == ',' || c == '(' || c == ')' || c == '*' || c == '-' || c == '+') {
            out.push_back({Token::Punct, std::string(1, c)});
            ++i;
        } else {
            fail("unexpected character '" + std::string(1, c) + "'");
        }
    }
    out.push_back({Token::End, ""});
    return out;
}

// ---- parsed query model -----------------------------------------------------------
struct Condition { std::string col; Cmp cmp; std::string value; bool forced_string; };
struct ParsedQuery {
    Agg                       agg = Agg::Count;
    std::string               metric;       // measure column (empty for COUNT)
    std::string               group;        // GROUP BY column (empty = ungrouped)
    std::vector<Condition>    where;
    bool                      order = false;
    bool                      order_desc = false;
    std::size_t               limit = 0;    // 0 = no limit
};

Cmp to_cmp(const std::string& op) {
    if (op == "=" || op == "==") return Cmp::Eq;
    if (op == "!=" || op == "<>") return Cmp::Ne;
    if (op == "<")  return Cmp::Lt;
    if (op == "<=") return Cmp::Le;
    if (op == ">")  return Cmp::Gt;
    if (op == ">=") return Cmp::Ge;
    fail("bad operator '" + op + "'");
}

std::optional<Agg> to_agg(const std::string& fn) {
    std::string f = upper(fn);
    if (f == "COUNT") return Agg::Count;
    if (f == "SUM")   return Agg::Sum;
    if (f == "AVG")   return Agg::Avg;
    if (f == "MIN")   return Agg::Min;
    if (f == "MAX")   return Agg::Max;
    return std::nullopt;
}

struct Parser {
    std::vector<Token> t;
    std::size_t p = 0;

    const Token& peek() const { return t[p]; }
    const Token& next() { return t[p++]; }
    bool is_kw(const std::string& kw) const { return peek().kind == Token::Ident && upper(peek().text) == kw; }
    bool accept_kw(const std::string& kw) { if (is_kw(kw)) { ++p; return true; } return false; }
    void expect_kw(const std::string& kw) { if (!accept_kw(kw)) fail("expected " + kw + " near '" + peek().text + "'"); }
    std::string ident() {
        if (peek().kind != Token::Ident) fail("expected a column name near '" + peek().text + "'");
        return next().text;
    }

    ParsedQuery parse() {
        ParsedQuery q;
        expect_kw("SELECT");
        bool have_agg = false;
        std::string dim_select;
        // select list: a mix of one aggregate and at most one bare dimension column.
        for (;;) {
            if (peek().kind == Token::Punct && peek().text == "*")
                fail("SELECT * is not supported — select an aggregate, e.g. COUNT(*)");
            std::string name = ident();
            if (peek().kind == Token::Punct && peek().text == "(") {        // aggregate function
                next();  // (
                auto a = to_agg(name);
                if (!a) fail("unknown function '" + name + "'");
                if (peek().kind == Token::Punct && peek().text == "*") {
                    next();
                    if (*a != Agg::Count) fail(upper(name) + "(*) is not valid; use " + upper(name) + "(column)");
                } else {
                    std::string col = ident();
                    if (*a != Agg::Count) q.metric = col;  // COUNT(col) ~ COUNT(*)
                }
                if (!(peek().kind == Token::Punct && peek().text == ")")) fail("expected ')'");
                next();  // )
                q.agg = *a; have_agg = true;
            } else {                                                        // bare dimension column
                if (!dim_select.empty()) fail("only one non-aggregated column is supported");
                dim_select = name;
            }
            if (peek().kind == Token::Punct && peek().text == ",") { next(); continue; }
            break;
        }

        expect_kw("FROM");
        ident();  // table name — accepted but ignored (query runs on the table you call)

        if (accept_kw("WHERE")) {
            for (;;) {
                Condition c;
                c.col = ident();
                if (peek().kind != Token::Op) fail("expected a comparison operator near '" + peek().text + "'");
                c.cmp = to_cmp(next().text);
                // value: optional leading sign, then number / string / bareword
                std::string sign;
                if (peek().kind == Token::Punct && (peek().text == "-" || peek().text == "+"))
                    sign = next().text == "-" ? "-" : "";
                const Token& v = next();
                if (v.kind == Token::Num)       { c.value = sign + v.text; c.forced_string = false; }
                else if (v.kind == Token::Str)  { c.value = v.text;        c.forced_string = true; }
                else if (v.kind == Token::Ident){ c.value = v.text;        c.forced_string = true; }  // unquoted, e.g. camp_5
                else fail("expected a value near '" + v.text + "'");
                q.where.push_back(std::move(c));
                if (accept_kw("AND")) continue;
                if (is_kw("OR")) fail("OR is not supported (conditions are AND-only)");
                break;
            }
        }

        if (accept_kw("GROUP")) { expect_kw("BY"); q.group = ident(); }
        else if (!have_agg && !dim_select.empty()) q.group = dim_select;  // SELECT col FROM t -> per-col COUNT

        if (q.group.empty() && !dim_select.empty() && have_agg) q.group = dim_select;  // implicit group

        if (accept_kw("ORDER")) {
            expect_kw("BY");
            next();                       // a column name or ordinal — we sort by the measure
            q.order = true;
            q.order_desc = false;         // SQL default is ASC
            if (accept_kw("DESC")) q.order_desc = true;
            else accept_kw("ASC");
        }

        if (accept_kw("LIMIT")) {
            if (peek().kind != Token::Num) fail("expected a number after LIMIT");
            q.limit = static_cast<std::size_t>(std::strtoull(next().text.c_str(), nullptr, 10));
        }

        if (peek().kind != Token::End) fail("unexpected trailing input near '" + peek().text + "'");
        return q;
    }
};

}  // namespace

QueryResult run_sql(const Table& table, const std::string& sql, unsigned threads) {
    ParsedQuery q = Parser{tokenize(sql)}.parse();

    if (q.agg != Agg::Count && q.metric.empty())
        fail("aggregate requires a column, e.g. SUM(cost)");

    Executor ex(table);
    ex.threads(threads);
    for (const auto& c : q.where) {
        if (!table.has_column(c.col)) fail("no such column '" + c.col + "'");
        const bool as_string = c.forced_string || table.column(c.col).type == DType::Dict;
        if (as_string) ex.filter(c.col, c.cmp, std::string_view(c.value));
        else ex.filter(c.col, c.cmp, std::strtod(c.value.c_str(), nullptr));
    }
    if (!q.group.empty()) {
        if (!table.has_column(q.group)) fail("no such column '" + q.group + "'");
        ex.group_by(q.group);
    }
    if (!q.metric.empty() && !table.has_column(q.metric)) fail("no such column '" + q.metric + "'");

    QueryResult r = ex.agg(q.agg, q.metric);

    if (q.order) r.sort_by_value(q.order_desc);
    if (q.limit && r.grouped && r.keys.size() > q.limit) {
        r.keys.resize(q.limit);
        if (!r.keys_num.empty()) r.keys_num.resize(q.limit);
        r.counts.resize(q.limit);
        r.values.resize(q.limit);
    }
    return r;
}

}  // namespace strata
