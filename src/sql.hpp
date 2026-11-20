#pragma once
// SQL front end: tokenizer, parser and expression evaluation. The dialect is the common
// core of MySQL, PostgreSQL and SQLite (see SQL.md). Rows are JSON documents; columns are
// (dotted) field names; every table has an "id" primary key.
#include "query.hpp"
#include "fulltext.hpp"
#include <memory>
#include <optional>
#include <variant>
#include <unordered_map>

namespace kestro::sql {
using json::Scalar;

[[noreturn]] inline void fail(const std::string& m) { throw std::runtime_error("INPUT SQL: " + m); }

// ---------- Tokens ----------
struct Token {
  enum Kind { End, Ident, Quoted, Number, String, Param, Op } kind = End;
  std::string text;  // identifiers keep their case; upper() for keyword checks
  size_t pos = 0, end = 0;  // source bytes [pos, end)
  size_t param = 0;  // 0-based index of a ? placeholder
};
inline std::string upper(std::string s) { for (auto& c : s) if (c >= 'a' && c <= 'z') c = char(c - 32); return s; }
// Placeholders: ? (in order), ?N or $N (numbered from 1), :name or @name (named; each name
// gets one index in order of first use, listed in *names). Named and unnamed ones do not mix.
inline std::vector<Token> tokenize(std::string_view s, std::vector<std::string>* names = nullptr) {
  std::vector<Token> out;
  size_t i = 0, params = 0;
  bool positional = false, numbered = false, named = false;
  auto mixed = [&] { if (named && (positional || numbered)) fail("named parameters (:name) cannot be mixed with ? or $N"); if (positional && numbered) fail("? and ?N/$N parameters cannot be mixed"); };
  auto ident_char = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$'; };
  while (i < s.size()) {
    char c = s[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
    if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') { while (i < s.size() && s[i] != '\n') i++; continue; }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') { auto e = s.find("*/", i + 2); if (e == std::string_view::npos) fail("unterminated comment"); i = e + 2; continue; }
    Token t; t.pos = i;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
      size_t j = i; while (j < s.size() && ident_char(s[j])) j++;
      t.kind = Token::Ident; t.text = std::string(s.substr(i, j - i)); i = j;
    } else if (c == '"' || c == '`') {
      size_t j = i + 1; std::string v;
      for (;; j++) { if (j >= s.size()) fail("unterminated quoted identifier"); if (s[j] == c) { if (j + 1 < s.size() && s[j + 1] == c) { v += c; j++; continue; } break; } v += s[j]; }
      t.kind = Token::Quoted; t.text = v; i = j + 1;
    } else if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9')) {
      size_t j = i; while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == '.')) j++;
      if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) { j++; if (j < s.size() && (s[j] == '+' || s[j] == '-')) j++; while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++; }
      t.kind = Token::Number; t.text = std::string(s.substr(i, j - i)); i = j;
    } else if (c == '\'') {
      size_t j = i + 1; std::string v;
      for (;; j++) { if (j >= s.size()) fail("unterminated string"); if (s[j] == '\'') { if (j + 1 < s.size() && s[j + 1] == '\'') { v += '\''; j++; continue; } break; } v += s[j]; }
      t.kind = Token::String; t.text = v; i = j + 1;
    } else if ((c == '?' || c == '$') && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') {
      size_t j = i + 1, n = 0; while (j < s.size() && s[j] >= '0' && s[j] <= '9') { n = n * 10 + size_t(s[j] - '0'); if (n > 65535) fail("parameter number too large"); j++; }
      if (n == 0) fail("parameters are numbered from 1");
      t.kind = Token::Param; t.param = n - 1; t.text = std::string(s.substr(i, j - i)); i = j; numbered = true; mixed();
    } else if (c == '?') { t.kind = Token::Param; t.param = params++; t.text = "?"; i++; positional = true; mixed(); }
    else if ((c == ':' || c == '@') && i + 1 < s.size() && ((s[i + 1] >= 'a' && s[i + 1] <= 'z') || (s[i + 1] >= 'A' && s[i + 1] <= 'Z') || s[i + 1] == '_')) {
      size_t j = i + 1; while (j < s.size() && ident_char(s[j]) && s[j] != '$') j++;
      std::string name(s.substr(i + 1, j - i - 1));
      if (!names) fail("named parameters are not supported here");
      size_t k = 0; while (k < names->size() && (*names)[k] != name) k++;
      if (k == names->size()) names->push_back(name);
      t.kind = Token::Param; t.param = k; t.text = std::string(s.substr(i, j - i)); i = j; named = true; mixed();
    }
    else {
      static const char* two[] = {"<=", ">=", "<>", "!=", "||", "=="};
      t.kind = Token::Op;
      bool done = false;
      for (auto op : two) if (s.substr(i, 2) == op) { t.text = op; i += 2; done = true; break; }
      if (!done) {
        if (!strchr("=<>+-*/%(),.;", c)) fail(std::string("unexpected character '") + c + "'");
        t.text = std::string(1, c); i++;
      }
    }
    t.end = i;
    out.push_back(std::move(t));
  }
  Token end; end.pos = end.end = s.size(); out.push_back(end);
  return out;
}

// ---------- Expressions ----------
struct Select;
struct Window;
struct Expr {
  enum Kind { Lit, Param, Col, Neg, Bin, And, Or, Not, In, Between, Like, IsNull, Func, Star, Subquery, Exists, Case } kind = Lit;
  std::shared_ptr<Select> sub;    // Subquery / Exists (resolved to literals before evaluation)
  std::string op;                 // Bin: = <> < <= > >= + - * / % ||; Func: upper-case name; Case: CASE (args: when, then, ...[, else]) or CASE_SIMPLE (args: operand, when, then, ...[, else])
  Scalar lit;                     // Lit
  size_t param = 0;               // Param
  std::vector<std::string> path;  // Col
  std::vector<Expr> args;         // children
  bool negate = false;            // NOT IN / NOT BETWEEN / NOT LIKE / IS NOT NULL
  bool insensitive = false;       // ILIKE
  bool distinct = false;          // COUNT(DISTINCT x)
  std::string text;               // source text, used as the default column name
  std::shared_ptr<Window> over;   // Func: a window function (... OVER (...))
  bool filter = false;            // aggregate FILTER (WHERE c): c is the last of args (so every walk of args sees it)
  uint16_t height = 1;            // levels of operators below and including this node (Parser::MAX_HEIGHT)
  bool boolean = false;           // MATCH (columns...) AGAINST (query IN BOOLEAN MODE): op MATCH, args the columns then the query
};
// OVER (PARTITION BY ... ORDER BY ... [frame]), as SQLite has it. The frame is ROWS, RANGE or
// GROUPS BETWEEN start AND end; each bound is UNBOUNDED PRECEDING/FOLLOWING, CURRENT ROW, or
// n PRECEDING/FOLLOWING. The default, RANGE UNBOUNDED PRECEDING .. CURRENT ROW, runs from the
// start of the partition to the current row's last peer (the whole partition without ORDER BY).
// frame is kept for older readers: ToCurrentRow (ROWS to the current row) or Whole.
struct Window { std::vector<Expr> partition; std::vector<std::pair<Expr, bool>> order; enum Frame { Default, ToCurrentRow, Whole } frame = Default;
  enum Unit { Rows, Range, Groups } unit = Range;
  enum Bound { UnboundedPreceding, Preceding, CurrentRow, Following, UnboundedFollowing } start = UnboundedPreceding, end = CurrentRow;
  std::optional<Expr> start_n, end_n;  // the n of n PRECEDING / n FOLLOWING
  bool framed = false;  // a frame clause was written
  std::string base;     // OVER w / OVER (w ...): the named window (WINDOW w AS ...) this builds on; the parser resolves it
  bool bare = false;    // OVER w (the named window as it is)
};
inline bool is_window_function(const std::string& f) { return f == "ROW_NUMBER" || f == "RANK" || f == "DENSE_RANK" || f == "NTILE" || f == "LAG" || f == "LEAD" || f == "FIRST_VALUE" || f == "LAST_VALUE" || f == "NTH_VALUE" || f == "PERCENT_RANK" || f == "CUME_DIST"; }
inline bool has_window(const Expr& e) {
  if (e.over) return true;
  for (const auto& a : e.args) if (has_window(a)) return true;
  return false;
}
inline bool is_aggregate(const std::string& f) { return f == "COUNT" || f == "SUM" || f == "AVG" || f == "MIN" || f == "MAX"; }
inline bool has_subquery(const Expr& e) {
  if (e.kind == Expr::Subquery || e.kind == Expr::Exists) return true;
  for (const auto& a : e.args) if (has_subquery(a)) return true;
  return false;
}
// Removes a leading table name or alias from column paths (a single-table statement).
inline void strip_qualifier(Expr& e, const std::string& a, const std::string& b) {
  if (e.kind == Expr::Col && e.path.size() > 1 && (e.path[0] == a || e.path[0] == b)) e.path.erase(e.path.begin());
  for (auto& x : e.args) strip_qualifier(x, a, b);
  if (e.over) { for (auto& p : e.over->partition) strip_qualifier(p, a, b); for (auto& o : e.over->order) strip_qualifier(o.first, a, b); }  // OVER (PARTITION BY t.c) too
}
inline bool is_match(const Expr& e) { return e.kind == Expr::Func && e.op == "MATCH"; }
inline bool has_match(const Expr& e) {
  if (is_match(e)) return true;
  for (const auto& a : e.args) if (has_match(a)) return true;
  if (e.over) { for (const auto& p : e.over->partition) if (has_match(p)) return true; for (const auto& o : e.over->order) if (has_match(o.first)) return true; }
  return false;
}
inline bool has_param(const Expr& e) {
  if (e.kind == Expr::Param) return true;
  for (const auto& a : e.args) if (has_param(a)) return true;
  return false;
}
inline bool has_aggregate(const Expr& e) {
  if (e.kind == Expr::Func && is_aggregate(e.op) && !e.over) return true;
  for (const auto& a : e.args) if (has_aggregate(a)) return true;
  // RANK() OVER (ORDER BY COUNT(*)) makes the query an aggregate, as in SQLite.
  if (e.over) { for (const auto& p : e.over->partition) if (has_aggregate(p)) return true; for (const auto& o : e.over->order) if (has_aggregate(o.first)) return true; }
  return false;
}

// ---------- Statements ----------
struct OrderItem { Expr e; bool desc = false; };
struct SelectItem { Expr e; std::string name; bool star = false; std::string star_alias; bool named = false; };
struct FromItem { std::string table, alias; enum Kind { Base, Inner, Left, Cross, Right, Full } kind = Base; std::optional<Expr> on;
  std::shared_ptr<Select> sub; };  // a derived table, FROM (SELECT ...) alias: table is then an internal name
// Names of tables that are not stored: derived tables ("\x01" + a number) and the one empty
// row a SELECT without FROM reads ("\x01"). Stored table names never start with \x01.
inline bool internal_table(const std::string& t) { return !t.empty() && t[0] == '\x01'; }
inline std::string no_table() { return std::string(1, '\x01'); }
struct Select;
// WITH name [(cols)] AS (SELECT ...): a common table expression of the SELECT it starts.
struct Cte { std::string name; std::vector<std::string> cols; std::shared_ptr<Select> q; };
// UNION [ALL] / INTERSECT [ALL] / EXCEPT [ALL] SELECT ... (applied left to right, as SQLite does).
struct SetPart { enum Op { Union, Intersect, Except } op = Union; bool all = false; std::shared_ptr<Select> q; };
struct Select {
  std::string table;            // the first table (all of FROM when there is no join)
  std::vector<SetPart> compound;  // further SELECTs; ORDER BY / LIMIT then apply to the whole result
  std::vector<FromItem> from;
  bool distinct = false;
  std::vector<SelectItem> items;
  std::optional<Expr> where, having, limit, offset;
  std::vector<Expr> group;
  std::vector<OrderItem> order;
  std::vector<std::pair<std::string, std::shared_ptr<Window>>> windows;  // WINDOW name AS (...), in order
  std::vector<Cte> with;        // WITH tables, visible to each other and to the whole SELECT
};
struct Insert { std::string table; std::vector<std::string> cols; std::vector<std::vector<Expr>> rows; bool replace = false, ignore = false; std::vector<std::pair<std::string, Expr>> on_conflict_set; bool on_conflict_nothing = false; };
struct Update { std::string table; std::vector<std::pair<std::string, Expr>> sets; std::optional<Expr> where; };
struct Delete { std::string table; std::optional<Expr> where; };
// Constraints checked at the end of every INSERT, UPDATE and DELETE (store_rules.inc).
// FOREIGN KEY cols REFERENCES table (ref) [ON DELETE/UPDATE action]; ref defaults to id.
struct ForeignKey {
  enum Action { NoAction, Restrict, Cascade, SetNull, SetDefault };
  std::string name, table; std::vector<std::string> cols, ref; Action on_delete = NoAction, on_update = NoAction;
};
inline const char* action_name(ForeignKey::Action a) { switch (a) { case ForeignKey::Restrict: return "RESTRICT"; case ForeignKey::Cascade: return "CASCADE"; case ForeignKey::SetNull: return "SET NULL"; case ForeignKey::SetDefault: return "SET DEFAULT"; default: return "NO ACTION"; } }
inline ForeignKey::Action action_of(const std::string& s) { return s == "RESTRICT" ? ForeignKey::Restrict : s == "CASCADE" ? ForeignKey::Cascade : s == "SET NULL" ? ForeignKey::SetNull : s == "SET DEFAULT" ? ForeignKey::SetDefault : ForeignKey::NoAction; }
struct CheckDef { std::string name, text; };                    // CHECK (text)
struct UniqueDef { std::string name; std::vector<std::string> cols; };  // UNIQUE (a, b, ...)
struct Constraints {
  std::vector<CheckDef> checks; std::vector<ForeignKey> fks; std::vector<UniqueDef> unique;
  bool empty() const { return checks.empty() && fks.empty() && unique.empty(); }
};
// A declared column. type is the enforced class: integer, number, text, boolean or any.
// rules: CHECK and REFERENCES written on the column (moved to the table's constraints).
// length: the n of VARCHAR(n) / CHAR(n), in characters (0: no limit).
struct ColumnDef { std::string name, type = "any"; bool not_null = false, unique = false, primary = false, has_default = false; Scalar def; Constraints rules; size_t length = 0; };
inline std::string type_class(const std::string& t) {
  static const char* integer[] = {"INT", "INTEGER", "BIGINT", "SMALLINT", "TINYINT", "MEDIUMINT", "SERIAL", "BIGSERIAL", "SMALLSERIAL", "INT2", "INT4", "INT8"};
  static const char* number[] = {"REAL", "FLOAT", "DOUBLE", "NUMERIC", "DECIMAL", "FLOAT4", "FLOAT8", "DEC", "FIXED"};
  static const char* text[] = {"TEXT", "VARCHAR", "CHAR", "CHARACTER", "STRING", "CLOB", "UUID", "DATE", "TIME", "DATETIME", "TIMESTAMP", "TIMESTAMPTZ", "NVARCHAR", "NCHAR", "TINYTEXT", "MEDIUMTEXT", "LONGTEXT", "CITEXT", "INTERVAL"};
  for (auto x : integer) if (t == x) return "integer";
  for (auto x : number) if (t == x) return "number";
  for (auto x : text) if (t == x) return "text";
  if (t == "BOOL" || t == "BOOLEAN") return "boolean";
  return "any";  // JSON, JSONB, BLOB, no type, anything else
}
// unique: single-column UNIQUE (unique indexes); rules: CHECK, FOREIGN KEY, UNIQUE over several columns.
// more: further columns (composite index). fulltext: CREATE FULLTEXT INDEX (min_length: WITH (min_token_length = n)).
struct CreateIndex { std::string name, table, column; bool unique = false, if_not_exists = false; std::vector<std::string> more; bool fulltext = false; size_t min_length = 1; };
struct CreateTable { std::string table; bool if_not_exists = false; std::vector<std::string> unique; std::vector<ColumnDef> columns; Constraints rules; std::vector<CreateIndex> fulltext; };  // fulltext: FULLTEXT (cols)
// RenameTable: to. AddConstraint: rules (or unique, one column). DropConstraint: from (a name).
// SetDefault ... Modify (keep them last) change the declared column from (ALTER [COLUMN], MODIFY);
// column holds the new default (SetDefault), type and length (SetType) or whole definition (Modify).
struct AlterTable { std::string table; enum Action { AddColumn, DropColumn, RenameColumn, RenameTable, AddConstraint, DropConstraint, SetDefault, DropDefault, SetNotNull, DropNotNull, SetType, Modify } action = AddColumn; ColumnDef column; std::string from, to; bool if_exists = false, if_not_exists = false; Constraints rules; std::vector<std::string> unique; std::vector<CreateIndex> fulltext; };  // fulltext: ADD FULLTEXT (AddConstraint)
struct DropTable { std::string table; bool if_exists = false; };
struct DropIndex { std::string name, table; bool if_exists = false; };
// snapshot: BEGIN CONCURRENT ISOLATION LEVEL SNAPSHOT (or REPEATABLE READ, READ COMMITTED): writes
// checked at COMMIT, reads not (the default is SERIALIZABLE: reads checked too).
struct Begin { bool concurrent = false, snapshot = false; }; struct Commit {}; struct Rollback {};
struct Savepoint { std::string name; }; struct Release { std::string name; }; struct RollbackTo { std::string name; };
// LISTEN channel / UNLISTEN channel | * (all: UNLISTEN *). NOTIFY channel [, payload] and
// SELECT pg_notify(channel, payload) (then channel_expr holds the channel). See store_notify.inc.
struct Listen { std::string channel; bool unlisten = false, all = false; };
struct Notify { std::string channel; std::optional<Expr> channel_expr, payload; bool select = false; };
// MySQL-style catalog statements.
struct ShowTables {};
struct Describe { std::string table; };
struct ShowIndexes { std::string table; };
using Statement = std::variant<Select, Insert, Update, Delete, CreateTable, DropTable, CreateIndex, DropIndex, AlterTable, Begin, Commit, Rollback, ShowTables, Describe, ShowIndexes, Savepoint, Release, RollbackTo, Listen, Notify>;

class Parser {
  std::string_view src_;
  std::vector<Token> t_;
  size_t i_ = 0;
  const Token& peek(size_t k = 0) const { return t_[std::min(i_ + k, t_.size() - 1)]; }
  bool kw(const char* w, size_t k = 0) const { const auto& t = peek(k); return t.kind == Token::Ident && upper(t.text) == w; }
  bool op(const char* o) const { return peek().kind == Token::Op && peek().text == o; }
  bool accept_kw(const char* w) { if (kw(w)) { i_++; return true; } return false; }
  bool accept_op(const char* o) { if (op(o)) { i_++; return true; } return false; }
  void expect_kw(const char* w) { if (!accept_kw(w)) fail(std::string("expected ") + w + location()); }
  void expect_op(const char* o) { if (!accept_op(o)) fail(std::string("expected '") + o + "'" + location()); }
  // Hostile or generated SQL must not exhaust the stack: parsing, evaluating and freeing an
  // expression recurse once per level. Nesting (parentheses, function calls, CASE, NOT,
  // unary minus, subqueries) is limited while parsing; the height of every expression tree
  // (operator chains built in loops included) when each node is made.
 public:
  static constexpr int MAX_NEST = 64, MAX_SELECT_NEST = 16, MAX_HEIGHT = 500;
 private:
  int nest_ = 0, select_nest_ = 0;
  int derived_ = 0;  // derived tables so far (their internal names)
  struct Nest {
    int& n;
    Nest(int& counter, int limit, const char* what) : n(counter) { if (++n > limit) { --n; fail(std::string("the statement nests ") + what + " too deeply (at most " + std::to_string(limit) + " levels)"); } }
    ~Nest() { --n; }
  };
  static void grow(Expr& e) {
    uint16_t h = 0;
    for (const auto& a : e.args) h = std::max(h, a.height);
    if (h + 1 > MAX_HEIGHT) fail("the expression is too large (at most " + std::to_string(MAX_HEIGHT) + " levels of operators; write long OR chains as IN (...))");
    e.height = uint16_t(h + 1);
  }
  std::string location() const { const auto& t = peek(); return t.kind == Token::End ? " at end" : " near '" + std::string(src_.substr(t.pos, 20)) + "'"; }
  static bool reserved(const std::string& u) {
    static const char* words[] = {"SELECT", "FROM", "WHERE", "AND", "OR", "NOT", "OVER", "UNION", "INTERSECT", "EXCEPT", "ORDER", "BY", "GROUP", "HAVING", "LIMIT", "OFFSET", "AS", "IN", "IS", "NULL", "LIKE", "ILIKE", "BETWEEN", "INSERT", "INTO", "VALUES", "UPDATE", "SET", "DELETE", "ASC", "DESC", "ON", "DISTINCT", "TRUE", "FALSE", "CREATE", "DROP", "TABLE", "INDEX", "UNIQUE", "JOIN", "INNER", "LEFT", "RIGHT", "FULL", "OUTER", "CROSS", "EXISTS", "NATURAL", "USING", "CASE", "WHEN", "THEN", "ELSE", "END"};
    for (auto w : words) if (u == w) return true;
    return false;
  }
  std::string name() {  // identifier (table, column part, alias)
    const auto& t = peek();
    if (t.kind == Token::Quoted || (t.kind == Token::Ident && !reserved(upper(t.text)))) { i_++; return t.text; }
    fail("expected a name" + location());
  }
  std::string dotted() { std::string n = name(); while (op(".")) { i_++; n += "." + name(); } return n; }
  std::string table_name() { auto n = name(); if (op(".")) { i_++; n = name(); } if (!query::valid_collection(n) || n[0] == '_' && n.size() > 1 && n[1] == '_') fail("invalid table name '" + n + "'"); return n; }
  std::string text_from(size_t start) const { size_t a = t_[start].pos, b = t_[i_ > 0 ? i_ - 1 : 0].pos; const auto& last = t_[i_ > 0 ? i_ - 1 : 0]; size_t len = last.kind == Token::String ? last.text.size() + 2 : last.kind == Token::Quoted ? last.text.size() + 2 : last.text.size(); return std::string(src_.substr(a, b + len - a)); }

  // The inside of OVER (...) or WINDOW w AS (...): [base window] [PARTITION BY] [ORDER BY] [frame].
  void window_spec(Window& w) {
    if ((peek().kind == Token::Ident || peek().kind == Token::Quoted) && !kw("PARTITION") && !kw("ORDER") && !kw("ROWS") && !kw("RANGE") && !kw("GROUPS")) w.base = name();
    if (accept_kw("PARTITION")) { expect_kw("BY"); do w.partition.push_back(expr()); while (accept_op(",")); }
    if (accept_kw("ORDER")) { expect_kw("BY"); do { Expr x = expr(); bool d = false; if (accept_kw("DESC")) d = true; else accept_kw("ASC"); w.order.emplace_back(std::move(x), d); } while (accept_op(",")); }
    if (kw("ROWS")) w.unit = Window::Rows; else if (kw("RANGE")) w.unit = Window::Range; else if (kw("GROUPS")) w.unit = Window::Groups; else return;
    i_++; w.framed = true;
    auto bound = [&](std::optional<Expr>& n) {
      if (accept_kw("UNBOUNDED")) { if (accept_kw("PRECEDING")) return Window::UnboundedPreceding; expect_kw("FOLLOWING"); return Window::UnboundedFollowing; }
      if (accept_kw("CURRENT")) { expect_kw("ROW"); return Window::CurrentRow; }
      n = binary_level(0);  // not expr(): BETWEEN's AND follows
      if (accept_kw("PRECEDING")) return Window::Preceding;
      expect_kw("FOLLOWING"); return Window::Following;
    };
    if (accept_kw("BETWEEN")) { w.start = bound(w.start_n); expect_kw("AND"); w.end = bound(w.end_n); }
    else { w.start = bound(w.start_n); if (w.start == Window::Following) fail("unsupported frame specification"); }
    // Bounds SQLite refuses (the frame would end before it starts).
    if (w.start == Window::UnboundedFollowing || w.end == Window::UnboundedPreceding || (w.start == Window::CurrentRow && w.end == Window::Preceding) || (w.start == Window::Following && (w.end == Window::Preceding || w.end == Window::CurrentRow))) fail("unsupported frame specification");
    if (kw("EXCLUDE")) fail("EXCLUDE in window frames is not supported");
    w.frame = w.start == Window::UnboundedPreceding && w.end == Window::UnboundedFollowing ? Window::Whole : w.unit == Window::Rows && w.start == Window::UnboundedPreceding && w.end == Window::CurrentRow ? Window::ToCurrentRow : Window::Default;
  }
  // OVER w and OVER (w ...) become the window WINDOW w AS (...) defines. As in SQLite, the
  // second form may add ORDER BY (when w has none) and a frame (when w has none), nothing else.
  static std::shared_ptr<Window> named_window(const std::vector<std::pair<std::string, std::shared_ptr<Window>>>& defs, const std::shared_ptr<Window>& w) {
    if (w->base.empty()) return w;
    std::shared_ptr<Window> b;
    for (const auto& d : defs) if (d.first == w->base) b = d.second;
    if (!b) fail("no such window: " + w->base);
    if (w->bare) return b;
    if (!w->partition.empty()) fail("cannot override PARTITION clause of window: " + w->base);
    if (!w->order.empty() && !b->order.empty()) fail("cannot override ORDER BY clause of window: " + w->base);
    if (b->framed) fail("cannot override frame specification of window: " + w->base);
    auto out = std::make_shared<Window>(*w);
    out->base.clear(); out->partition = b->partition; if (out->order.empty()) out->order = b->order;
    return out;
  }
  static void resolve_windows(Expr& e, const std::vector<std::pair<std::string, std::shared_ptr<Window>>>& defs) {
    if (e.over) e.over = named_window(defs, e.over);
    for (auto& a : e.args) resolve_windows(a, defs);  // not e.sub: a subquery resolved its own when it was parsed
  }
  Expr primary() {
    Nest guard(nest_, MAX_NEST, "parentheses, function calls and CASE");
    size_t start = i_;
    Expr e;
    const auto& t = peek();
    if (t.kind == Token::Number) { e.kind = Expr::Lit; e.lit = json::scalar_of(t.text); if (e.lit.tag != json::NUMBER) fail("bad number " + t.text); i_++; }
    else if (t.kind == Token::String) { e.kind = Expr::Lit; e.lit.tag = json::STRING; e.lit.str = t.text; i_++; }
    else if (t.kind == Token::Param) { e.kind = Expr::Param; e.param = t.param; i_++; }
    else if (accept_kw("NULL")) { e.kind = Expr::Lit; e.lit.tag = json::NUL; }
    else if (accept_kw("TRUE")) { e.kind = Expr::Lit; e.lit.tag = json::TRUE_; }
    else if (accept_kw("FALSE")) { e.kind = Expr::Lit; e.lit.tag = json::FALSE_; }
    else if (accept_kw("CASE")) {
      e.kind = Expr::Case; e.op = "CASE";
      if (!kw("WHEN")) { e.op = "CASE_SIMPLE"; e.args.push_back(expr()); }
      if (!kw("WHEN")) fail("CASE needs WHEN ... THEN ..." + location());
      while (accept_kw("WHEN")) { e.args.push_back(expr()); expect_kw("THEN"); e.args.push_back(expr()); }
      if (accept_kw("ELSE")) e.args.push_back(expr());
      expect_kw("END");
    }
    else if (accept_op("(")) {
      if (accept_kw("SELECT")) { e.kind = Expr::Subquery; e.sub = std::make_shared<Select>(select()); }
      else e = expr();
      expect_op(")");
    }
    else if (kw("EXISTS") && peek(1).kind == Token::Op && peek(1).text == "(") {
      i_ += 2; expect_kw("SELECT"); e.kind = Expr::Exists; e.sub = std::make_shared<Select>(select()); expect_op(")");
    }
    else if (op("*")) { i_++; e.kind = Expr::Star; }
    else if (t.kind == Token::Ident && upper(t.text) == "MATCH" && peek(1).kind == Token::Op && peek(1).text == "(") {
      // MATCH (col, ...) AGAINST (text [IN NATURAL LANGUAGE MODE | IN BOOLEAN MODE])
      i_ += 2; e.kind = Expr::Func; e.op = "MATCH";
      do { size_t cs = i_; Expr c; c.kind = Expr::Col; c.path = json::split_path(dotted()); c.text = text_from(cs); grow(c); e.args.push_back(std::move(c)); } while (accept_op(","));
      expect_op(")");
      if (e.args.size() > 8) fail("MATCH names at most 8 columns");
      expect_kw("AGAINST"); expect_op("(");
      e.args.push_back(binary_level(0));
      if (accept_kw("IN")) {
        if (accept_kw("BOOLEAN")) e.boolean = true; else { expect_kw("NATURAL"); expect_kw("LANGUAGE"); }
        expect_kw("MODE");
      }
      if (kw("WITH")) fail("MATCH ... AGAINST supports IN NATURAL LANGUAGE MODE and IN BOOLEAN MODE (not WITH QUERY EXPANSION)");
      expect_op(")");
    }
    else if ((t.kind == Token::Ident || t.kind == Token::Quoted) && peek(1).kind == Token::Op && peek(1).text == "(" && t.kind == Token::Ident) {
      e.kind = Expr::Func; e.op = upper(t.text); i_ += 2;
      if (accept_kw("DISTINCT")) e.distinct = true;
      if (!op(")")) { do e.args.push_back(op("*") ? (i_++, Expr{Expr::Star}) : expr()); while (accept_op(",")); }
      expect_op(")");
      if (kw("FILTER") && peek(1).kind == Token::Op && peek(1).text == "(") {
        if (!is_aggregate(e.op)) fail("FILTER needs an aggregate function, not " + e.op);
        i_ += 2; expect_kw("WHERE"); e.args.push_back(expr()); expect_op(")"); e.filter = true;
      }
      if (accept_kw("OVER")) {
        if (!is_aggregate(e.op) && !is_window_function(e.op)) fail(e.op + " is not a window function");
        if (e.distinct) fail("DISTINCT is not supported for window functions");
        auto w = std::make_shared<Window>();
        if (!op("(")) { w->base = name(); w->bare = true; }
        else { expect_op("("); window_spec(*w); expect_op(")"); }
        e.over = w;
      } else if (is_window_function(e.op)) fail(e.op + " needs OVER (...)");
    } else { e.kind = Expr::Col; std::string d = dotted(); e.path = json::split_path(d); }
    e.text = text_from(start);
    grow(e);
    return e;
  }
  Expr unary() {
    size_t s = i_;
    if (accept_op("-")) { Nest guard(nest_, MAX_NEST, "unary minus"); Expr e; e.kind = Expr::Neg; e.args.push_back(unary()); e.text = text_from(s); grow(e); return e; }
    if (accept_op("+")) { Nest guard(nest_, MAX_NEST, "unary plus"); return unary(); }
    return primary();
  }
  Expr binary_level(int level) {
    // 0: || ; 1: + - ; 2: * / %
    if (level > 2) return unary();
    size_t s = i_;
    Expr left = binary_level(level + 1);
    for (;;) {
      const char* ops0[] = {"||"}; const char* ops1[] = {"+", "-"}; const char* ops2[] = {"*", "/", "%"};
      std::string o;
      if (level == 0 && op("||")) o = "||"; else if (level == 1 && (op("+") || op("-"))) o = peek().text; else if (level == 2 && (op("*") || op("/") || op("%"))) o = peek().text;
      (void)ops0; (void)ops1; (void)ops2;
      if (o.empty()) { if (left.text.empty()) left.text = text_from(s); return left; }
      i_++;
      Expr e; e.kind = Expr::Bin; e.op = o; e.args.push_back(std::move(left)); e.args.push_back(binary_level(level + 1)); grow(e);
      left = std::move(e);
    }
  }
  Expr comparison() {
    size_t s = i_;
    Expr left = binary_level(0);
    for (const char* o : {"=", "==", "<>", "!=", "<=", ">=", "<", ">"}) if (op(o)) {
      i_++; Expr e; e.kind = Expr::Bin; e.op = o; if (e.op == "==") e.op = "="; if (e.op == "!=") e.op = "<>";
      e.args.push_back(std::move(left)); e.args.push_back(binary_level(0)); e.text = text_from(s); grow(e); return e;
    }
    bool negate = false;
    if (kw("NOT") && (kw("IN", 1) || kw("LIKE", 1) || kw("ILIKE", 1) || kw("BETWEEN", 1))) { i_++; negate = true; }
    if (accept_kw("IN")) {
      Expr e; e.kind = Expr::In; e.negate = negate; e.args.push_back(std::move(left)); expect_op("(");
      if (accept_kw("SELECT")) { Expr q; q.kind = Expr::Subquery; q.sub = std::make_shared<Select>(select()); e.args.push_back(std::move(q)); }
      else do e.args.push_back(binary_level(0)); while (accept_op(","));
      expect_op(")"); e.text = text_from(s); grow(e); return e;
    }
    if (kw("LIKE") || kw("ILIKE")) {
      Expr e; e.kind = Expr::Like; e.insensitive = kw("ILIKE"); i_++; e.negate = negate;
      e.args.push_back(std::move(left)); e.args.push_back(binary_level(0)); e.text = text_from(s); grow(e); return e;
    }
    if (accept_kw("BETWEEN")) {
      Expr e; e.kind = Expr::Between; e.negate = negate; e.args.push_back(std::move(left)); e.args.push_back(binary_level(0)); expect_kw("AND"); e.args.push_back(binary_level(0)); e.text = text_from(s); grow(e); return e;
    }
    if (accept_kw("IS")) {
      Expr e; e.kind = Expr::IsNull; e.negate = accept_kw("NOT"); expect_kw("NULL"); e.args.push_back(std::move(left)); e.text = text_from(s); grow(e); return e;
    }
    if (negate) fail("expected IN, LIKE or BETWEEN after NOT" + location());
    return left;
  }
  Expr not_level() { size_t s = i_; if (accept_kw("NOT")) { Nest guard(nest_, MAX_NEST, "NOT"); Expr e; e.kind = Expr::Not; e.args.push_back(not_level()); e.text = text_from(s); grow(e); return e; } return comparison(); }
  Expr and_level() { size_t s = i_; Expr l = not_level(); bool chain = false; while (accept_kw("AND")) { Expr e; e.kind = Expr::And; e.args.push_back(std::move(l)); e.args.push_back(not_level()); grow(e); l = std::move(e); chain = true; } if (chain) l.text = text_from(s); return l; }
  Expr expr() { size_t s = i_; Expr l = and_level(); bool chain = false; while (accept_kw("OR")) { Expr e; e.kind = Expr::Or; e.args.push_back(std::move(l)); e.args.push_back(and_level()); grow(e); l = std::move(e); chain = true; } if (chain) l.text = text_from(s); return l; }

  // A SELECT, with any UNION / INTERSECT / EXCEPT parts; ORDER BY and LIMIT end it.
  Select select() {
    Nest guard(select_nest_, MAX_SELECT_NEST, "subqueries");
    Select q = select_core();
    for (;;) {
      SetPart p;
      if (accept_kw("UNION")) p.op = SetPart::Union; else if (accept_kw("INTERSECT")) p.op = SetPart::Intersect; else if (accept_kw("EXCEPT")) p.op = SetPart::Except; else break;
      if (accept_kw("ALL")) p.all = true; else accept_kw("DISTINCT");
      expect_kw("SELECT");
      p.q = std::make_shared<Select>(select_core());
      q.compound.push_back(std::move(p));
    }
    if (accept_kw("ORDER")) { expect_kw("BY"); do { OrderItem o; o.e = expr(); if (accept_kw("DESC")) o.desc = true; else accept_kw("ASC"); q.order.push_back(std::move(o)); } while (accept_op(",")); }
    if (accept_kw("LIMIT")) { Expr a = binary_level(0); if (accept_op(",")) { q.offset = a; q.limit = binary_level(0); } else q.limit = a; }
    if (accept_kw("OFFSET")) q.offset = binary_level(0);
    for (auto& o : q.order) resolve_windows(o.e, q.windows);
    if (q.compound.empty() && q.from.size() == 1) for (auto& o : q.order) strip_qualifier(o.e, q.from[0].alias, q.from[0].alias);
    return q;
  }
  Select select_core() {
    Select q;
    if (accept_kw("DISTINCT")) q.distinct = true; else accept_kw("ALL");
    do {
      SelectItem it;
      if (op("*")) { i_++; it.star = true; it.name = "*"; }
      else if ((peek().kind == Token::Ident || peek().kind == Token::Quoted) && peek(1).kind == Token::Op && peek(1).text == "." && peek(2).kind == Token::Op && peek(2).text == "*") {
        it.star = true; it.star_alias = peek().text; it.name = it.star_alias + ".*"; i_ += 3;
      } else {
        it.e = expr(); it.name = it.e.kind == Expr::Col ? it.e.text : it.e.text;
        if (it.e.kind == Expr::Col) { std::string n; for (size_t k = 0; k < it.e.path.size(); k++) n += (k ? "." : "") + it.e.path[k]; it.name = n; }
        if (accept_kw("AS")) { it.name = name(); it.named = true; }
        else if (peek().kind == Token::Quoted || (peek().kind == Token::Ident && !reserved(upper(peek().text)))) { it.name = name(); it.named = true; }
      }
      q.items.push_back(std::move(it));
    } while (accept_op(","));
    if (!accept_kw("FROM")) return select_without_from(std::move(q));
    auto table_ref = [&](FromItem::Kind kind) {
      FromItem f; f.kind = kind;
      if (op("(") && (kw("SELECT", 1) || kw("WITH", 1))) { i_++; f.sub = std::make_shared<Select>(subselect()); expect_op(")"); f.table = no_table() + std::to_string(++derived_); }
      else f.table = table_name();
      if (accept_kw("AS")) f.alias = name();
      else if (peek().kind == Token::Quoted || (peek().kind == Token::Ident && !reserved(upper(peek().text)) && !(kw("WINDOW") && kw("AS", 2)))) f.alias = name();
      if (f.alias.empty()) f.alias = f.table;
      return f;
    };
    q.from.push_back(table_ref(FromItem::Base));
    for (;;) {
      if (accept_op(",")) { q.from.push_back(table_ref(FromItem::Cross)); continue; }
      if (accept_kw("CROSS")) { expect_kw("JOIN"); q.from.push_back(table_ref(FromItem::Cross)); continue; }
      FromItem::Kind k;
      if (accept_kw("LEFT")) { accept_kw("OUTER"); expect_kw("JOIN"); k = FromItem::Left; }
      else if (accept_kw("RIGHT")) { accept_kw("OUTER"); expect_kw("JOIN"); k = FromItem::Right; }
      else if (accept_kw("FULL")) { accept_kw("OUTER"); expect_kw("JOIN"); k = FromItem::Full; }
      else if (accept_kw("INNER")) { expect_kw("JOIN"); k = FromItem::Inner; }
      else if (accept_kw("JOIN")) k = FromItem::Inner;
      else if (kw("NATURAL")) fail("NATURAL joins are not supported; write JOIN ... USING (columns) or ON");
      else break;
      auto f = table_ref(k);
      if (accept_kw("USING")) {
        // USING (a, b): the previous table's a = this table's a, and so on.
        const std::string prev = q.from.back().alias;
        expect_op("(");
        do {
          auto c = name();
          Expr l; l.kind = Expr::Col; l.path = {prev, c}; l.text = prev + "." + c;
          Expr r; r.kind = Expr::Col; r.path = {f.alias, c}; r.text = f.alias + "." + c;
          Expr eq; eq.kind = Expr::Bin; eq.op = "="; eq.args.push_back(std::move(l)); eq.args.push_back(std::move(r));
          if (!f.on) f.on = std::move(eq); else { Expr a; a.kind = Expr::And; a.args.push_back(std::move(*f.on)); a.args.push_back(std::move(eq)); f.on = std::move(a); }
        } while (accept_op(","));
        expect_op(")");
      } else { expect_kw("ON"); f.on = expr(); }
      q.from.push_back(std::move(f));
    }
    if (q.from.size() > 16) fail("at most 16 tables in one query");
    for (size_t i = 0; i < q.from.size(); i++) for (size_t j = 0; j < i; j++) if (q.from[i].alias == q.from[j].alias) fail("table name or alias " + q.from[i].alias + " is used twice; give each an alias");
    q.table = q.from[0].table;
    if (accept_kw("WHERE")) q.where = expr();
    if (accept_kw("GROUP")) { expect_kw("BY"); do q.group.push_back(expr()); while (accept_op(",")); }
    if (accept_kw("HAVING")) q.having = expr();
    if (accept_kw("WINDOW")) {
      do { auto n = name(); expect_kw("AS"); expect_op("("); auto w = std::make_shared<Window>(); window_spec(*w); expect_op(")"); q.windows.emplace_back(n, named_window(q.windows, w)); } while (accept_op(","));
    }
    for (auto& it : q.items) if (!it.star) resolve_windows(it.e, q.windows);
    // One table: "users.name" and "u.name" mean "name". Joins keep qualified paths.
    if (q.from.size() == 1) {
      const auto &a = q.from[0].alias, &t = a;  // an aliased table is known only by its alias
      for (auto& it : q.items) if (!it.star) strip_qualifier(it.e, a, t); else if (it.star_alias == a || it.star_alias == t) it.star_alias.clear();
      if (q.where) strip_qualifier(*q.where, a, t);
      if (q.having) strip_qualifier(*q.having, a, t);
      for (auto& g : q.group) strip_qualifier(g, a, t);
    }
    // Unnamed columns are named after the column (without its table), as in MySQL.
    for (auto& it : q.items) if (!it.star && !it.named && it.e.kind == Expr::Col) {
      size_t from = 0;
      if (q.from.size() > 1 && it.e.path.size() > 1) for (const auto& f : q.from) if (f.alias == it.e.path[0]) from = 1;
      std::string n; for (size_t k = from; k < it.e.path.size(); k++) n += (k > from ? "." : "") + it.e.path[k];
      it.name = n;
    }
    return q;
  }
  // SELECT without FROM (SELECT 1, SELECT ? + 1): one row with no columns.
  Select select_without_from(Select q) {
    for (const auto& it : q.items) if (it.star) fail("SELECT * needs FROM" + location());
    FromItem f; f.table = f.alias = no_table(); q.from.push_back(std::move(f)); q.table = q.from[0].table;
    if (accept_kw("WHERE")) q.where = expr();
    if (accept_kw("GROUP")) { expect_kw("BY"); do q.group.push_back(expr()); while (accept_op(",")); }
    if (accept_kw("HAVING")) q.having = expr();
    return q;
  }
  // A SELECT in parentheses (derived table or WITH body), which may start with its own WITH.
  Select subselect() { if (accept_kw("WITH")) return with_query(); expect_kw("SELECT"); return select(); }
  // After WITH: [RECURSIVE] name [(cols)] AS (SELECT ...) [, ...] SELECT ... A WITH table
  // naming itself is recursive (as in SQLite, RECURSIVE is optional).
  Select with_query() {
    Nest guard(select_nest_, MAX_SELECT_NEST, "subqueries");
    accept_kw("RECURSIVE");
    std::vector<Cte> ctes;
    do {
      Cte c; c.name = table_name();
      for (const auto& d : ctes) if (d.name == c.name) fail("WITH defines " + c.name + " twice");
      if (accept_op("(")) {
        do { c.cols.push_back(name()); for (size_t k = 0; k + 1 < c.cols.size(); k++) if (c.cols[k] == c.cols.back()) fail("WITH " + c.name + " lists " + c.cols.back() + " twice"); } while (accept_op(","));
        expect_op(")");
      }
      expect_kw("AS");
      if (accept_kw("NOT")) expect_kw("MATERIALIZED"); else accept_kw("MATERIALIZED");
      expect_op("("); c.q = std::make_shared<Select>(subselect()); expect_op(")");
      ctes.push_back(std::move(c));
    } while (accept_op(","));
    expect_kw("SELECT");
    Select q = select(); q.with = std::move(ctes);
    return q;
  }
  // type [(args)] (several words: DOUBLE PRECISION, CHARACTER VARYING(n)): sets c.type, and
  // c.length for VARCHAR(n) / CHAR(n). Returns whether a type was written.
  bool column_type(ColumnDef& c) {
    auto constraint = [&](const std::string& u) {
      static const char* words[] = {"PRIMARY", "UNIQUE", "NOT", "NULL", "DEFAULT", "CHECK", "REFERENCES", "COLLATE", "CONSTRAINT", "AUTO_INCREMENT", "AUTOINCREMENT", "GENERATED", "USING"};
      for (auto w : words) if (u == w) return true;
      return false;
    };
    std::string type;
    while (peek().kind == Token::Ident && !constraint(upper(peek().text))) {
      if (type.empty()) type = upper(peek().text);
      i_++;
      if (op("(")) {
        bool chars = type == "VARCHAR" || type == "CHAR" || type == "CHARACTER" || type == "NVARCHAR" || type == "NCHAR";
        if (chars && peek(1).kind == Token::Number && peek(2).kind == Token::Op && peek(2).text == ")") {
          const auto& n = peek(1).text;
          if (n.find_first_not_of("0123456789") != std::string::npos || n.size() > 8 || std::stoul(n) == 0) fail("the length of " + type + "(n) must be a whole number from 1");
          c.length = std::stoul(n);
        }
        int depth = 0; do { if (op("(")) depth++; else if (op(")")) depth--; i_++; } while (depth > 0 && peek().kind != Token::End);
      }
    }
    c.type = type_class(type);
    return !type.empty();
  }
  // DEFAULT's value: a constant (DEFAULT NULL: no default).
  void default_value(ColumnDef& c) {
    bool paren = accept_op("(");
    Expr e = unary();
    if (e.kind == Expr::Neg && e.args[0].kind == Expr::Lit && e.args[0].lit.tag == json::NUMBER) { c.def = e.args[0].lit; c.def.num = -c.def.num; }
    else if (e.kind == Expr::Lit) c.def = e.lit;
    else fail("DEFAULT must be a constant (a number, text, TRUE, FALSE or NULL)");
    if (paren) expect_op(")");
    c.has_default = c.def.tag != json::NUL;
  }
  // name [type [(args)]] [PRIMARY KEY] [UNIQUE] [NOT NULL | NULL] [DEFAULT literal] ...
  ColumnDef column_def() {
    ColumnDef c; c.name = name();
    std::string pending_name;
    column_type(c);
    for (;;) {
      if (accept_kw("PRIMARY")) { expect_kw("KEY"); c.primary = true; accept_kw("ASC"); accept_kw("DESC"); }
      else if (accept_kw("UNIQUE")) { c.unique = true; accept_kw("KEY"); }
      else if (accept_kw("NOT")) { expect_kw("NULL"); c.not_null = true; }
      else if (accept_kw("NULL")) {}
      else if (accept_kw("AUTO_INCREMENT") || accept_kw("AUTOINCREMENT")) {}
      else if (accept_kw("DEFAULT")) default_value(c);
      else if (accept_kw("CONSTRAINT")) { pending_name = name(); continue; }
      else if (accept_kw("CHECK")) c.rules.checks.push_back({pending_name, check_text()});
      else if (accept_kw("REFERENCES")) { auto f = references(); f.name = pending_name; f.cols = {c.name}; c.rules.fks.push_back(std::move(f)); }
      else if (accept_kw("COLLATE")) name();
      else if (kw("GENERATED") || kw("ON")) {
        int depth = 0;  // accepted, not enforced
        while (peek().kind != Token::End && !(depth == 0 && (op(",") || op(")")))) { if (op("(")) depth++; else if (op(")")) depth--; i_++; }
      } else break;
      pending_name.clear();
    }
    if (c.primary && c.name != "id") fail("the primary key must be the id column");
    return c;
  }
  // CHECK (expr): the expression's source text (kept in the catalog and parsed again).
  std::string check_text() {
    expect_op("(");
    size_t a = i_;
    Expr e = expr();
    if (!op(")")) fail("expected ) after the CHECK expression" + location());
    std::string text(src_.substr(t_[a].pos, t_[i_ - 1].end - t_[a].pos));
    i_++;
    if (has_subquery(e) || has_aggregate(e) || has_param(e) || has_window(e) || has_match(e)) fail("CHECK takes an expression of the row's own columns (no subqueries, aggregates or parameters)");
    return text;
  }
  std::vector<std::string> name_list() {
    std::vector<std::string> out; expect_op("(");
    do { out.push_back(name()); if (accept_op("(")) { while (!op(")") && peek().kind != Token::End) i_++; expect_op(")"); } if (!accept_kw("ASC")) accept_kw("DESC"); } while (accept_op(","));
    expect_op(")"); return out;
  }
  // After REFERENCES: table [(cols)] [ON DELETE action] [ON UPDATE action] [MATCH ...] [DEFERRABLE ...].
  ForeignKey references() {
    ForeignKey f; f.table = table_name();
    if (op("(")) f.ref = name_list();
    for (;;) {
      if (kw("ON") && (kw("DELETE", 1) || kw("UPDATE", 1))) {
        i_++; bool del = accept_kw("DELETE"); if (!del) expect_kw("UPDATE");
        ForeignKey::Action a;
        if (accept_kw("CASCADE")) a = ForeignKey::Cascade;
        else if (accept_kw("RESTRICT")) a = ForeignKey::Restrict;
        else if (accept_kw("NO")) { expect_kw("ACTION"); a = ForeignKey::NoAction; }
        else if (accept_kw("SET")) { if (accept_kw("NULL")) a = ForeignKey::SetNull; else { expect_kw("DEFAULT"); a = ForeignKey::SetDefault; } }
        else fail("ON DELETE / ON UPDATE take CASCADE, RESTRICT, NO ACTION, SET NULL or SET DEFAULT" + location());
        (del ? f.on_delete : f.on_update) = a;
      } else if (accept_kw("MATCH")) { i_++; }
      else if (kw("NOT") && kw("DEFERRABLE", 1)) { i_ += 2; }
      else if (accept_kw("DEFERRABLE")) {}
      else if (accept_kw("INITIALLY")) { if (!accept_kw("DEFERRED")) expect_kw("IMMEDIATE"); }
      else break;
    }
    return f;
  }
  bool constraint_start() const { return kw("PRIMARY") || kw("UNIQUE") || kw("CONSTRAINT") || kw("FOREIGN") || kw("CHECK") || kw("KEY") || kw("INDEX"); }
  // A table constraint. Single-column UNIQUE goes to unique (a unique index); PRIMARY KEY must
  // be (id); KEY/INDEX (MySQL inline indexes) are accepted and ignored.
  void table_constraint(Constraints& rules, std::vector<std::string>& unique) {
    std::string cname;
    if (accept_kw("CONSTRAINT")) cname = name();
    if (accept_kw("CHECK")) { rules.checks.push_back({cname, check_text()}); return; }
    if (accept_kw("FOREIGN")) {
      expect_kw("KEY"); if (!op("(")) name();
      auto cols = name_list(); expect_kw("REFERENCES");
      auto f = references(); f.name = cname; f.cols = std::move(cols);
      if (!f.ref.empty() && f.ref.size() != f.cols.size()) fail("FOREIGN KEY has " + std::to_string(f.cols.size()) + " columns but references " + std::to_string(f.ref.size()));
      rules.fks.push_back(std::move(f)); return;
    }
    if (accept_kw("UNIQUE")) {
      if (!accept_kw("KEY")) accept_kw("INDEX");
      if (!op("(")) { auto n = name(); if (cname.empty()) cname = n; }
      auto cols = name_list();
      for (size_t i = 0; i < cols.size(); i++) for (size_t j = 0; j < i; j++) if (cols[i] == cols[j]) fail("UNIQUE lists " + cols[i] + " twice");
      if (cols.size() == 1) unique.push_back(cols[0]); else rules.unique.push_back({cname, std::move(cols)});
      return;
    }
    if (accept_kw("PRIMARY")) {
      expect_kw("KEY"); auto cols = name_list();
      if (cols.size() != 1 || cols[0] != "id") fail("the primary key must be the id column");
      return;
    }
    if (!accept_kw("KEY")) expect_kw("INDEX");
    int depth = 0;
    while (peek().kind != Token::End) {
      if (op("(")) { depth++; i_++; continue; }
      if (op(")")) { if (depth == 0) break; depth--; i_++; continue; }
      if (op(",") && depth == 0) break;
      i_++;
    }
  }
  // After FULLTEXT: [INDEX | KEY] [name] (columns) [WITH (min_token_length = n)]; statement:
  // CREATE FULLTEXT INDEX [IF NOT EXISTS] [name] ON table (columns) [WITH (...)].
  CreateIndex fulltext_def(bool statement = false) {
    CreateIndex s; s.fulltext = true;
    if (!accept_kw("INDEX")) accept_kw("KEY");
    if (statement) { if (accept_kw("IF")) { expect_kw("NOT"); expect_kw("EXISTS"); s.if_not_exists = true; } if (!kw("ON")) s.name = name(); expect_kw("ON"); s.table = table_name(); }
    else if (!op("(")) s.name = name();
    expect_op("("); s.column = dotted(); if (!accept_kw("ASC")) accept_kw("DESC");
    while (accept_op(",")) { s.more.push_back(dotted()); if (!accept_kw("ASC")) accept_kw("DESC"); }
    if (s.more.size() > 7) fail("an index covers at most 8 columns");
    std::vector<std::string> cols{s.column}; cols.insert(cols.end(), s.more.begin(), s.more.end());
    for (size_t i = 0; i < cols.size(); i++) {
      if (cols[i] == "id") fail("a FULLTEXT index covers text columns, not id");
      for (size_t j = 0; j < i; j++) if (cols[i] == cols[j]) fail("the index lists " + cols[i] + " twice");
    }
    expect_op(")");
    if (accept_kw("WITH")) {
      expect_op("(");
      do {
        auto o = upper(name()); expect_op("=");
        if (o != "MIN_TOKEN_LENGTH") fail("FULLTEXT indexes take WITH (min_token_length = n), not " + o);
        const auto& n = peek();
        if (n.kind != Token::Number || n.text.find_first_not_of("0123456789") != std::string::npos || n.text.size() > 2 || std::stoul(n.text) < 1 || std::stoul(n.text) > 64) fail("min_token_length must be a whole number from 1 to 64");
        s.min_length = std::stoul(n.text); i_++;
      } while (accept_op(","));
      expect_op(")");
    }
    return s;
  }
  std::vector<std::pair<std::string, Expr>> assignments() {
    std::vector<std::pair<std::string, Expr>> sets;
    do { std::string c = dotted(); expect_op("="); sets.emplace_back(c, expr()); } while (accept_op(","));
    return sets;
  }

 public:
  explicit Parser(std::string_view s, std::vector<std::string>* names = nullptr) : src_(s), t_(tokenize(s, names)) {}
  Expr whole_expression() { Expr e = expr(); if (peek().kind != Token::End) fail("unexpected text after the expression" + location()); return e; }
  Statement statement() {
    Statement st = statement_inner();
    accept_op(";");
    if (peek().kind != Token::End) fail("unexpected text after the statement" + location() + " (run one statement per call)");
    return st;
  }
  // A channel: a name, a "quoted" name (any text, such as "table_changes:users") or a 'string'.
  std::string channel() { if (peek().kind == Token::String) return t_[i_++].text; return name(); }
  Statement statement_inner() {
    if (kw("SELECT") && kw("PG_NOTIFY", 1) && peek(2).kind == Token::Op && peek(2).text == "(") {
      i_ += 3; Notify n; n.select = true; n.channel_expr = expr(); if (accept_op(",")) n.payload = expr(); expect_op(")"); return n;
    }
    if (accept_kw("SELECT")) return select();
    if (accept_kw("LISTEN")) return Listen{channel()};
    if (accept_kw("UNLISTEN")) { if (accept_op("*")) return Listen{std::string(), true, true}; return Listen{channel(), true}; }
    if (accept_kw("NOTIFY")) { Notify n; n.channel = channel(); if (accept_op(",")) n.payload = expr(); return n; }
    if (accept_kw("WITH")) return with_query();
    if (kw("INSERT") || kw("REPLACE")) {
      Insert s;
      if (accept_kw("REPLACE")) s.replace = true;
      else { i_++; if (accept_kw("OR")) { if (accept_kw("REPLACE")) s.replace = true; else { expect_kw("IGNORE"); s.ignore = true; } } else if (accept_kw("IGNORE")) s.ignore = true; }
      expect_kw("INTO"); s.table = table_name();
      if (accept_op("(")) { do s.cols.push_back(dotted()); while (accept_op(",")); expect_op(")"); }
      expect_kw("VALUES");
      do { expect_op("("); std::vector<Expr> row; do row.push_back(expr()); while (accept_op(",")); expect_op(")"); s.rows.push_back(std::move(row)); } while (accept_op(","));
      if (accept_kw("ON")) {
        if (accept_kw("CONFLICT")) {  // PostgreSQL / SQLite
          if (accept_op("(")) { do name(); while (accept_op(",")); expect_op(")"); }
          expect_kw("DO");
          if (accept_kw("NOTHING")) s.on_conflict_nothing = true; else { expect_kw("UPDATE"); expect_kw("SET"); s.on_conflict_set = assignments(); }
        } else { expect_kw("DUPLICATE"); expect_kw("KEY"); expect_kw("UPDATE"); s.on_conflict_set = assignments(); }  // MySQL
      }
      return s;
    }
    if (accept_kw("UPDATE")) {
      Update s; s.table = table_name(); expect_kw("SET"); s.sets = assignments(); if (accept_kw("WHERE")) s.where = expr();
      if (s.where) strip_qualifier(*s.where, s.table, s.table);
      for (auto& [c, e] : s.sets) strip_qualifier(e, s.table, s.table);
      return s;
    }
    if (accept_kw("DELETE")) { Delete s; expect_kw("FROM"); s.table = table_name(); if (accept_kw("WHERE")) s.where = expr(); if (s.where) strip_qualifier(*s.where, s.table, s.table); return s; }
    if (accept_kw("BEGIN") || accept_kw("START")) {
      Begin b; b.concurrent = accept_kw("CONCURRENT"); accept_kw("TRANSACTION");
      // A plain BEGIN is serializable at any level asked for (it holds the write gate); weaker
      // levels give BEGIN CONCURRENT its snapshot alone (a stronger level is allowed, as in SQL).
      if (accept_kw("ISOLATION")) {
        expect_kw("LEVEL");
        if (accept_kw("SERIALIZABLE")) {}
        else if (accept_kw("SNAPSHOT")) b.snapshot = true;
        else if (accept_kw("REPEATABLE")) { expect_kw("READ"); b.snapshot = true; }
        else if (accept_kw("READ")) { if (!accept_kw("COMMITTED")) expect_kw("UNCOMMITTED"); b.snapshot = true; }
        else fail("expected SERIALIZABLE, SNAPSHOT, REPEATABLE READ or READ COMMITTED" + location());
        b.snapshot = b.snapshot && b.concurrent;
      }
      return b;
    }
    if (accept_kw("COMMIT") || accept_kw("END")) { accept_kw("TRANSACTION"); return Commit{}; }
    if (accept_kw("ROLLBACK")) {
      if (!accept_kw("TRANSACTION")) accept_kw("WORK");
      if (accept_kw("TO")) { accept_kw("SAVEPOINT"); return RollbackTo{name()}; }
      return Rollback{};
    }
    if (accept_kw("SAVEPOINT")) return Savepoint{name()};
    if (accept_kw("RELEASE")) { accept_kw("SAVEPOINT"); return Release{name()}; }
    if (accept_kw("CREATE")) {
      if (accept_kw("TABLE")) {
        CreateTable s;
        if (accept_kw("IF")) { expect_kw("NOT"); expect_kw("EXISTS"); s.if_not_exists = true; }
        s.table = table_name();
        if (accept_op("(")) {
          for (;;) {
            if (accept_kw("FULLTEXT")) { s.fulltext.push_back(fulltext_def()); s.fulltext.back().table = s.table; }
            else if (constraint_start()) table_constraint(s.rules, s.unique);
            else s.columns.push_back(column_def());
            if (accept_op(",")) continue;
            expect_op(")"); break;
          }
          for (auto& c : s.columns) {
            if (c.unique && !c.primary) s.unique.push_back(c.name);
            for (auto& x : c.rules.checks) s.rules.checks.push_back(std::move(x));
            for (auto& x : c.rules.fks) s.rules.fks.push_back(std::move(x));
            c.rules = Constraints{};
          }
          for (size_t i = 0; i < s.columns.size(); i++) for (size_t j = 0; j < i; j++) if (s.columns[i].name == s.columns[j].name) fail("column " + s.columns[i].name + " is declared twice");
        }
        while (peek().kind != Token::End && !op(";")) i_++;  // table options (ENGINE=..., WITHOUT ROWID)
        return s;
      }
      if (accept_kw("FULLTEXT")) { if (!kw("INDEX") && !kw("KEY")) fail("expected INDEX after CREATE FULLTEXT" + location()); return fulltext_def(true); }
      CreateIndex s;
      if (accept_kw("UNIQUE")) s.unique = true;
      expect_kw("INDEX");
      if (accept_kw("IF")) { expect_kw("NOT"); expect_kw("EXISTS"); s.if_not_exists = true; }
      if (!kw("ON")) s.name = name();
      expect_kw("ON"); s.table = table_name();
      expect_op("("); s.column = dotted(); if (accept_kw("ASC") || accept_kw("DESC")) {}
      while (accept_op(",")) { s.more.push_back(dotted()); if (accept_kw("ASC") || accept_kw("DESC")) {} }
      if (s.more.size() > 7) fail("an index covers at most 8 columns");
      expect_op(")");
      return s;
    }
    if (accept_kw("SHOW")) {
      if (accept_kw("TABLES")) return ShowTables{};
      if (accept_kw("COLUMNS") || accept_kw("FIELDS")) { if (!accept_kw("FROM")) expect_kw("IN"); Describe d; d.table = table_name(); return d; }
      if (accept_kw("INDEX") || accept_kw("INDEXES") || accept_kw("KEYS")) { if (!accept_kw("FROM")) expect_kw("IN"); ShowIndexes d; d.table = table_name(); return d; }
      fail("SHOW supports TABLES, COLUMNS FROM t and INDEX FROM t");
    }
    if (accept_kw("DESCRIBE") || accept_kw("DESC") || accept_kw("EXPLAIN")) { Describe d; d.table = table_name(); return d; }
    if (accept_kw("ALTER")) {
      expect_kw("TABLE"); AlterTable s;
      if (accept_kw("IF")) { expect_kw("EXISTS"); s.if_exists = true; }
      s.table = table_name();
      bool add = accept_kw("ADD"), drop = !add && accept_kw("DROP");
      if (add && accept_kw("FULLTEXT")) { s.action = AlterTable::AddConstraint; s.fulltext.push_back(fulltext_def()); s.fulltext.back().table = s.table; }
      else if (add && constraint_start()) {
        s.action = AlterTable::AddConstraint; table_constraint(s.rules, s.unique);
        if (s.rules.empty() && s.unique.empty()) fail("ALTER TABLE ADD takes CHECK, FOREIGN KEY or UNIQUE constraints");
      } else if (add) {
        accept_kw("COLUMN"); s.action = AlterTable::AddColumn;
        if (accept_kw("IF")) { expect_kw("NOT"); expect_kw("EXISTS"); s.if_not_exists = true; }
        s.column = column_def();
        if (s.column.primary) fail("the primary key is always id");
        s.rules = std::move(s.column.rules); s.column.rules = Constraints{};
      } else if (drop && (kw("CONSTRAINT") || kw("CHECK") || (kw("FOREIGN") && kw("KEY", 1)) || kw("INDEX") || kw("KEY"))) {
        i_ += kw("FOREIGN") ? 2 : 1;  // the kind; any constraint or index is found by its name
        s.action = AlterTable::DropConstraint;
        if (accept_kw("IF")) { expect_kw("EXISTS"); s.if_exists = true; }
        s.from = name();
      } else if (drop) {
        accept_kw("COLUMN"); s.action = AlterTable::DropColumn;
        if (accept_kw("IF")) { expect_kw("EXISTS"); s.if_exists = true; }
        s.from = dotted();
        if (s.from == "id") fail("the id column cannot be dropped");
      } else if (accept_kw("RENAME")) {
        if (accept_kw("TO") || accept_kw("AS")) { s.action = AlterTable::RenameTable; s.to = table_name(); return s; }
        accept_kw("COLUMN"); s.action = AlterTable::RenameColumn; s.from = dotted(); expect_kw("TO"); s.to = dotted();
        if (s.from == "id" || s.to == "id") fail("the id column cannot be renamed");
      } else if (accept_kw("ALTER")) {
        // ALTER [COLUMN] c SET DEFAULT x | DROP DEFAULT | SET NOT NULL | DROP NOT NULL | [SET DATA] TYPE t
        accept_kw("COLUMN"); s.from = dotted(); s.column.name = s.from;
        if (accept_kw("SET")) {
          if (accept_kw("DEFAULT")) { s.action = AlterTable::SetDefault; default_value(s.column); }
          else if (accept_kw("NOT")) { expect_kw("NULL"); s.action = AlterTable::SetNotNull; }
          else { expect_kw("DATA"); expect_kw("TYPE"); s.action = AlterTable::SetType; }
        } else if (accept_kw("DROP")) {
          if (accept_kw("DEFAULT")) s.action = AlterTable::DropDefault;
          else { expect_kw("NOT"); expect_kw("NULL"); s.action = AlterTable::DropNotNull; }
        } else if (accept_kw("TYPE")) s.action = AlterTable::SetType;
        else fail("ALTER COLUMN supports SET DEFAULT, DROP DEFAULT, SET NOT NULL, DROP NOT NULL and TYPE" + location());
        if (s.action == AlterTable::SetType) {
          if (!column_type(s.column)) fail("expected a type after TYPE" + location());
          if (kw("USING")) fail("ALTER COLUMN ... TYPE converts the values itself; USING is not supported");
          if (accept_kw("COLLATE")) name();
        }
        if (s.from == "id") fail("the id column cannot be changed");
      } else if (accept_kw("MODIFY")) {
        // MySQL: the column's whole new definition (type, NOT NULL, DEFAULT).
        accept_kw("COLUMN"); s.action = AlterTable::Modify; s.column = column_def(); s.from = s.column.name;
        if (s.from == "id" || s.column.primary) fail("the id column cannot be changed");
        if (!s.column.rules.empty()) fail("MODIFY does not add constraints; use ALTER TABLE ADD CONSTRAINT");
        if (!accept_kw("FIRST") && accept_kw("AFTER")) dotted();  // column order is not kept
      } else fail("ALTER TABLE supports ADD [COLUMN], ADD CONSTRAINT, DROP [COLUMN], DROP CONSTRAINT, RENAME COLUMN, RENAME TO, ALTER [COLUMN] and MODIFY" + location());
      return s;
    }
    if (accept_kw("DROP")) {
      if (accept_kw("TABLE")) { DropTable s; if (accept_kw("IF")) { expect_kw("EXISTS"); s.if_exists = true; } s.table = table_name(); return s; }
      expect_kw("INDEX"); DropIndex s;
      if (accept_kw("IF")) { expect_kw("EXISTS"); s.if_exists = true; }
      s.name = dotted();
      if (accept_kw("ON")) s.table = table_name();
      return s;
    }
    fail("unsupported statement" + location());
  }
};
inline Statement parse(std::string_view s) { return Parser(s).statement(); }
inline Expr parse_expression(std::string_view s) { return Parser(s).whole_expression(); }
// text with every identifier from (a column; or, with qualifier, a table followed by ".")
// replaced by "to" (CHECK texts after RENAME COLUMN / RENAME TO).
inline std::string rename_identifier(const std::string& text, const std::string& from, const std::string& to, bool qualifier) {
  auto t = tokenize(text); std::string out; size_t at = 0;
  for (size_t i = 0; i + 1 < t.size(); i++) {
    if ((t[i].kind != Token::Ident && t[i].kind != Token::Quoted) || t[i].text != from) continue;
    bool dot_after = t[i + 1].kind == Token::Op && t[i + 1].text == ".";
    bool call = t[i + 1].kind == Token::Op && t[i + 1].text == "(";
    bool dot_before = i > 0 && t[i - 1].kind == Token::Op && t[i - 1].text == ".";
    if (qualifier ? (!dot_after || dot_before) : (call || (dot_after && !dot_before && i + 2 < t.size()))) continue;
    out.append(text, at, t[i].pos - at);
    std::string q = "\""; for (char c : to) { if (c == '"') q += '"'; q += c; } q += '"';
    out += q; at = t[i].end;
  }
  out.append(text, at, std::string::npos);
  return out;
}
// A statement and the names of its :name parameters (index = position).
struct Parsed { Statement st; std::vector<std::string> names; };
inline Parsed parse_named(std::string_view s) { Parsed p; p.st = Parser(s, &p.names).statement(); return p; }

// ---------- Evaluation ----------
// SQL values are JSON scalars. NULL and a missing field are both NULL. Comparisons follow
// SQL: anything compared with NULL is NULL (false in WHERE); values of different types
// are never equal and never ordered (strict, like PostgreSQL).
// Column values from somewhere other than the JSON document (the scan column cache).
struct ColumnSource { virtual const Scalar* column(const Expr& col) const = 0; virtual ~ColumnSource() = default; };
struct Row {
  std::string_view doc;                   // the row's JSON document
  const std::vector<json::Scalar>* params = nullptr;  // converted once per statement
  const std::unordered_map<const Expr*, Scalar>* aggregates = nullptr;  // computed per group
  std::string_view excluded;  // INSERT ... ON CONFLICT: the row that was proposed
  const ColumnSource* columns = nullptr;  // when set, columns come from here, not doc
  // Joins: doc is {"alias": row-or-null, ...}; an unqualified column is looked up in
  // each table in FROM order.
  const std::vector<std::string>* aliases = nullptr;
};
inline Scalar null_value() { Scalar s; s.tag = json::NUL; return s; }
inline Scalar boolean(bool b) { Scalar s; s.tag = b ? json::TRUE_ : json::FALSE_; return s; }
inline bool is_null(const Scalar& s) { return s.tag == json::MISSING || s.tag == json::NUL; }
inline bool truthy(const Scalar& s) { return s.tag == json::TRUE_ || (s.tag == json::NUMBER && s.num != 0); }
inline Scalar number(double v) { Scalar s; s.tag = json::NUMBER; s.num = v == 0 ? 0.0 : v; return s; }
inline std::string to_json(const Scalar& s) {
  switch (s.tag) {
    case json::NUL: case json::MISSING: return "null";
    case json::TRUE_: return "true";
    case json::FALSE_: return "false";
    case json::NUMBER: {
      if (std::fabs(s.num) < 9007199254740992.0 && s.num == std::floor(s.num)) return std::to_string((long long)s.num);
      char b[40]; snprintf(b, sizeof b, "%.17g", s.num); return b;
    }
    case json::STRING: return json::quote(s.str);
    default: return s.str;  // composite: raw JSON
  }
}
inline bool like(std::string_view s, std::string_view p, bool ci) {
  auto lower = [&](char c) { return ci && c >= 'A' && c <= 'Z' ? char(c + 32) : c; };
  size_t si = 0, pi = 0, star = std::string_view::npos, mark = 0;
  while (si < s.size()) {
    if (pi < p.size() && (p[pi] == '_' || lower(p[pi]) == lower(s[si]))) { si++; pi++; }
    else if (pi < p.size() && p[pi] == '%') { star = pi++; mark = si; }
    else if (star != std::string_view::npos) { pi = star + 1; si = ++mark; }
    else return false;
  }
  while (pi < p.size() && p[pi] == '%') pi++;
  return pi == p.size();
}
inline Scalar param_value(const Expr& e, const Row& r) {
  if (!r.params || e.param >= r.params->size()) fail("missing value for parameter " + std::to_string(e.param + 1));
  return (*r.params)[e.param];
}
// Correlated subqueries (they name columns of the outer query) stay in the expression; the
// store evaluates them for each row through this hook, set while such a statement runs.
struct SubqueryHook { virtual std::vector<Scalar> values(const Expr& node, const Row& r, bool exists) const = 0; virtual ~SubqueryHook() = default; };
inline const SubqueryHook*& subquery_hook() { static thread_local const SubqueryHook* h = nullptr; return h; }
inline const SubqueryHook& require_hook() { if (!subquery_hook()) fail("internal: subquery was not resolved"); return *subquery_hook(); }
// MATCH ... AGAINST: the store prepares each one of a statement (query, index statistics) and
// finds it here by its node while the statement runs.
struct FulltextHook { virtual const fts::Prepared* find(const Expr& match) const = 0; virtual ~FulltextHook() = default; };
inline const FulltextHook*& fulltext_hook() { static thread_local const FulltextHook* h = nullptr; return h; }
inline Scalar eval(const Expr& e, const Row& r) {
  switch (e.kind) {
    case Expr::Lit: return e.lit;
    case Expr::Param: return param_value(e, r);
    case Expr::Col: {
      if (r.columns) { if (const Scalar* v = r.columns->column(e)) { if (v->tag == json::MISSING) return null_value(); return *v; } }
      if (!r.excluded.empty() && e.path.size() > 1 && e.path[0] == "excluded") { auto s = json::field(r.excluded, std::vector<std::string>(e.path.begin() + 1, e.path.end())); if (s.tag == json::MISSING) s.tag = json::NUL; return s; }
      if (r.aliases) {
        bool qualified = false;
        for (const auto& a : *r.aliases) if (e.path.size() > 1 && e.path[0] == a) qualified = true;
        if (!qualified) {
          std::vector<std::string> p; p.reserve(e.path.size() + 1);
          for (const auto& a : *r.aliases) {
            p.assign(1, a); p.insert(p.end(), e.path.begin(), e.path.end());
            auto v = json::field(r.doc, p);
            if (v.tag != json::MISSING) return v;
          }
          return null_value();
        }
      }
      auto s = json::field(r.doc, e.path); if (s.tag == json::MISSING) s.tag = json::NUL; return s; }
    case Expr::Neg: { auto v = eval(e.args[0], r); if (is_null(v)) return null_value(); if (v.tag != json::NUMBER) fail("cannot negate a non-number"); return number(-v.num); }
    case Expr::Not: { auto v = eval(e.args[0], r); if (is_null(v)) return null_value(); return boolean(!truthy(v)); }
    case Expr::And: { auto a = eval(e.args[0], r); if (!is_null(a) && !truthy(a)) return boolean(false); auto b = eval(e.args[1], r); if (!is_null(b) && !truthy(b)) return boolean(false); if (is_null(a) || is_null(b)) return null_value(); return boolean(true); }
    case Expr::Or: { auto a = eval(e.args[0], r); if (truthy(a)) return boolean(true); auto b = eval(e.args[1], r); if (truthy(b)) return boolean(true); if (is_null(a) || is_null(b)) return null_value(); return boolean(false); }
    case Expr::IsNull: { bool n = is_null(eval(e.args[0], r)); return boolean(e.negate ? !n : n); }
    case Expr::Case: {
      // The first WHEN that is true (searched) or equal to the operand (simple); else ELSE or NULL.
      bool simple = e.op == "CASE_SIMPLE"; size_t k = simple ? 1 : 0;
      Scalar v; if (simple) v = eval(e.args[0], r);
      for (; k + 1 < e.args.size(); k += 2) {
        auto w = eval(e.args[k], r);
        bool hit = simple ? !is_null(v) && !is_null(w) && query::type_class(v.tag) == query::type_class(w.tag) && json::compare(v, w) == 0 : truthy(w);
        if (hit) return eval(e.args[k + 1], r);
      }
      return k < e.args.size() ? eval(e.args[k], r) : null_value();
    }
    case Expr::In: {
      auto v = eval(e.args[0], r); if (is_null(v)) return null_value();
      bool found = false, saw_null = false;
      const bool sub = e.args.size() == 2 && e.args[1].kind == Expr::Subquery;
      std::vector<Scalar> listed; if (sub) listed = require_hook().values(e.args[1], r, false);
      size_t n = sub ? listed.size() : e.args.size() - 1;
      for (size_t k = 0; k < n; k++) { auto x = sub ? listed[k] : eval(e.args[k + 1], r); if (is_null(x)) saw_null = true; else if (json::compare(v, x) == 0) { found = true; break; } }
      if (!found && saw_null) return null_value();
      return boolean(e.negate ? !found : found);
    }
    case Expr::Between: {
      auto v = eval(e.args[0], r), lo = eval(e.args[1], r), hi = eval(e.args[2], r);
      if (is_null(v) || is_null(lo) || is_null(hi)) return null_value();
      if (query::type_class(v.tag) != query::type_class(lo.tag) || query::type_class(v.tag) != query::type_class(hi.tag)) return boolean(e.negate);
      bool in = json::compare(v, lo) >= 0 && json::compare(v, hi) <= 0;
      return boolean(e.negate ? !in : in);
    }
    case Expr::Like: {
      auto v = eval(e.args[0], r), p = eval(e.args[1], r);
      if (is_null(v) || is_null(p)) return null_value();
      if (v.tag != json::STRING || p.tag != json::STRING) return boolean(e.negate);
      bool m = like(v.str, p.str, e.insensitive);
      return boolean(e.negate ? !m : m);
    }
    case Expr::Bin: {
      auto a = eval(e.args[0], r), b = eval(e.args[1], r);
      const std::string& o = e.op;
      if (o == "||") { if (is_null(a) || is_null(b)) return null_value(); auto txt = [](const Scalar& s) { return s.tag == json::STRING ? s.str : to_json(s); }; Scalar s; s.tag = json::STRING; s.str = txt(a) + txt(b); return s; }
      if (is_null(a) || is_null(b)) return null_value();
      if (o == "+" || o == "-" || o == "*" || o == "/" || o == "%") {
        if (a.tag != json::NUMBER || b.tag != json::NUMBER) fail("arithmetic needs numbers: " + (e.text.empty() ? "operator " + o : e.text));
        if (o == "+") return number(a.num + b.num);
        if (o == "-") return number(a.num - b.num);
        if (o == "*") return number(a.num * b.num);
        if (b.num == 0) return null_value();  // division by zero is NULL (MySQL, SQLite)
        if (o == "/") return number(a.num / b.num);
        return number(std::fmod(a.num, b.num));
      }
      if (query::type_class(a.tag) != query::type_class(b.tag)) return boolean(o == "<>");
      int c = json::compare(a, b);
      if (o == "=") return boolean(c == 0);
      if (o == "<>") return boolean(c != 0);
      if (o == "<") return boolean(c < 0);
      if (o == "<=") return boolean(c <= 0);
      if (o == ">") return boolean(c > 0);
      return boolean(c >= 0);
    }
    case Expr::Func: {
      const auto& f = e.op;
      if (e.over) { if (r.aggregates) { auto it = r.aggregates->find(&e); if (it != r.aggregates->end()) return it->second; } fail("window function " + f + " is not allowed here"); }
      if (is_aggregate(f)) { if (r.aggregates) { auto it = r.aggregates->find(&e); if (it != r.aggregates->end()) return it->second; } fail("aggregate " + f + " is not allowed here"); }
      if (f == "VALUES" && !r.excluded.empty() && e.args.size() == 1 && e.args[0].kind == Expr::Col) { auto s = json::field(r.excluded, e.args[0].path); if (s.tag == json::MISSING) s.tag = json::NUL; return s; }
      if (f == "MATCH") {  // the row's relevance: its text values tokenized as the index has them
        const fts::Prepared* p = fulltext_hook() ? fulltext_hook()->find(e) : nullptr;
        if (!p) fail("MATCH ... AGAINST works in SELECT, UPDATE and DELETE on one table with a FULLTEXT index on its columns (not in joins, INSERT or CHECK)");
        static thread_local fts::Tokens toks; toks.clear();
        for (size_t k = 0; k + 1 < e.args.size(); k++) {
          auto v = eval(e.args[k], r); if (v.tag != json::STRING) continue;
          toks.separate();
          fts::tokens(v.str, p->min_len, [&](std::string_view t) { toks.add(t); });
        }
        return number(fts::score(p->query, p->stats, toks));
      }
      auto arg = [&](size_t k) { if (k >= e.args.size()) fail(f + " needs more arguments"); return eval(e.args[k], r); };
      if (f == "LOWER" || f == "UPPER") { auto v = arg(0); if (v.tag != json::STRING) return v; for (auto& c : v.str) { if (f == "LOWER" && c >= 'A' && c <= 'Z') c = char(c + 32); if (f == "UPPER" && c >= 'a' && c <= 'z') c = char(c - 32); } return v; }
      if (f == "LENGTH" || f == "CHAR_LENGTH") { auto v = arg(0); if (is_null(v)) return null_value(); if (v.tag != json::STRING) fail("LENGTH needs text"); size_t n = 0; for (unsigned char c : v.str) if ((c & 0xC0) != 0x80) n++; return number(double(n)); }
      if (f == "ABS") { auto v = arg(0); if (v.tag != json::NUMBER) return v; return number(std::fabs(v.num)); }
      if (f == "ROUND") { auto v = arg(0); if (v.tag != json::NUMBER) return v; double d = e.args.size() > 1 ? arg(1).num : 0; double m = std::pow(10.0, d); return number(std::round(v.num * m) / m); }
      if (f == "COALESCE" || f == "IFNULL") { for (size_t k = 0; k < e.args.size(); k++) { auto v = eval(e.args[k], r); if (!is_null(v)) return v; } return null_value(); }
      fail("unknown function " + f);
    }
    case Expr::Star: fail("* is not allowed here");
    case Expr::Subquery: { auto v = require_hook().values(e, r, false); if (v.size() > 1) fail("a subquery used as a single value returned more than one row"); return v.empty() ? null_value() : v[0]; }
    case Expr::Exists: return boolean(!require_hook().values(e, r, true).empty());
  }
  return null_value();
}

}  // namespace kestro::sql
