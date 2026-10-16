#include "googlesql/ffi/helpers.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "googlesql/parser/tm_token.h"
#include "googlesql/parser/token_with_location.h"
#include "googlesql/parser/tokenizer.h"

namespace googlesql::ffi {
namespace {

using ::googlesql::parser::GoogleSqlTokenizer;
using ::googlesql::parser::Token;
using ::googlesql::parser::TokenWithLocation;

// Next non-comment token, or nullopt if the tokenizer fails.
std::optional<TokenWithLocation> NextSignificant(GoogleSqlTokenizer& tokenizer) {
  while (true) {
    absl::StatusOr<TokenWithLocation> token = tokenizer.GetNextToken();
    if (!token.ok())
      return std::nullopt;
    if (token->kind != Token::COMMENT)
      return *token;
  }
}

// True for reserved and non-reserved keywords.
// Same test as IsKeywordToken in lookahead_transformer.cc, which is file-local there.
bool IsKeyword(Token kind) {
  return (kind > Token::SENTINEL_RESERVED_KW_START && kind < Token::SENTINEL_RESERVED_KW_END) ||
         (kind > Token::SENTINEL_NONRESERVED_KW_START && kind < Token::SENTINEL_NONRESERVED_KW_END);
}

// Kind of `token`, or EOI if there is none (end of input or a tokenizer failure).
Token KindOf(const std::optional<TokenWithLocation>& token) {
  return token.has_value() ? token->kind : Token::EOI;
}

// True if `token`, followed by `next` and `after_next`, is a script label, as in `lbl: BEGIN`
// (same rule as IsCurrentTokenScriptLabel in lookahead_transformer.cc).
// `e` and `e1` come from the tokenizer as exponent tokens, which the parser accepts as names.
bool IsScriptLabel(Token token, Token next, Token after_next) {
  if (next != Token::COLON)
    return false;
  if (token != Token::IDENTIFIER && token != Token::EXP_IN_FLOAT_NO_SIGN &&
      token != Token::STANDALONE_EXPONENT_SIGN && !IsKeyword(token))
    return false;
  switch (after_next) {
    case Token::KW_BEGIN:
    case Token::KW_WHILE:
    case Token::KW_LOOP:
    case Token::KW_REPEAT:
    case Token::KW_FOR:
      return true;
    default:
      return false;
  }
}

// True if PROCEDURE right after `prev` is what CREATE creates:
// CREATE [OR REPLACE] [TEMP|TEMPORARY|PUBLIC|PRIVATE] PROCEDURE.
bool StartsProcedure(Token prev) {
  switch (prev) {
    case Token::KW_CREATE:
    case Token::KW_REPLACE:
    case Token::KW_TEMP:
    case Token::KW_TEMPORARY:
    case Token::KW_PUBLIC:
    case Token::KW_PRIVATE:
      return true;
    default:
      return false;
  }
}

// BEGIN followed by these is BEGIN [TRANSACTION] [modes], not a block.
bool BeginsTransaction(const std::optional<TokenWithLocation>& next) {
  if (!next.has_value())
    return true;
  switch (next->kind) {
    case Token::EOI:
    case Token::SEMICOLON:
    case Token::KW_TRANSACTION:
    case Token::KW_ISOLATION:
    case Token::KW_READ:
      return true;
    default:
      return false;
  }
}
}  // namespace

std::pair<int, int> TokenRangeAt(absl::string_view sql, int offset) {
  GoogleSqlTokenizer tokenizer(/*filename=*/"", sql, offset);
  std::optional<TokenWithLocation> token = NextSignificant(tokenizer);
  if (!token.has_value()) {
    // Any of \n, \r\n, \r ends the line (same as ParseLocationTranslator).
    size_t eol = sql.find_first_of("\r\n", offset);
    if (eol == absl::string_view::npos)
      eol = sql.size();
    return {offset, static_cast<int>(eol)};
  }
  if (token->kind == Token::EOI) {
    return {static_cast<int>(sql.size()), static_cast<int>(sql.size())};
  }
  return {token->location.start().GetByteOffset(), token->location.end().GetByteOffset()};
}

bool NothingSignificantFrom(absl::string_view sql, int offset) {
  GoogleSqlTokenizer tokenizer(/*filename=*/"", sql, offset);
  std::optional<TokenWithLocation> token = NextSignificant(tokenizer);
  return token.has_value() && token->kind == Token::EOI;
}

int SkipStatement(absl::string_view sql, int stmt_start, int error_offset) {
  enum class Frame { kBlock, kCaseExpr };
  // Where a CREATE ... PROCEDURE header is. The parameter list is required, so its `(` ends
  // the name, and the body is the first depth-0 BEGIN after it: a procedure named `begin` is
  // not taken for one.
  enum class ProcHeader { kNone, kName, kAfterName };
  std::vector<Frame> frames;
  int paren_depth = 0;
  // True when the previous token lets a new statement begin here.
  bool after_boundary = true;
  Token prev = Token::EOI;  // EOI stands for "no previous token".
  // WHILE or FOR that opened a block and still waits for its DO; EOI if none.
  Token awaiting_do = Token::EOI;
  ProcHeader proc_header = ProcHeader::kNone;

  GoogleSqlTokenizer tokenizer(/*filename=*/"", sql, stmt_start);
  // The token after `previous`, or nullopt if `previous` is end of input or a tokenizer failure.
  auto read_after =
      [&tokenizer](
          const std::optional<TokenWithLocation>& previous) -> std::optional<TokenWithLocation> {
    if (KindOf(previous) == Token::EOI)
      return std::nullopt;
    return NextSignificant(tokenizer);
  };
  std::optional<TokenWithLocation> token = NextSignificant(tokenizer);
  std::optional<TokenWithLocation> next = read_after(token);
  while (token.has_value() && token->kind != Token::EOI) {
    std::optional<TokenWithLocation> after_next = read_after(next);
    Token kind = token->kind;
    // A keyword after `.`, `@` or `@@` is a name (`t.end`, `@end`), as in the lookahead
    // transformer. `prev` keeps this kind, so `t.end if` is never END IF.
    if ((prev == Token::DOT || prev == Token::ATSIGN || prev == Token::KW_DOUBLE_AT) &&
        IsKeyword(kind))
      kind = Token::IDENTIFIER;
    const bool at_statement_start =
        after_boundary && paren_depth == 0 && (frames.empty() || frames.back() != Frame::kCaseExpr);
    // Labels go before the keyword rules, since a keyword such as LOOP can be one
    // (`loop: LOOP`). The labeled statement starts at the block keyword after the `:`.
    if (at_statement_start && IsScriptLabel(kind, KindOf(next), KindOf(after_next))) {
      prev = Token::COLON;
      token = std::move(after_next);
      next = read_after(token);
      continue;
    }
    bool opened_block = false;
    bool opened_loop_body = false;

    // A keyword right after END closes (END IF, END CASE, ...); never opens.
    if (prev != Token::KW_END) {
      switch (kind) {
        case Token::KW_BEGIN:
          // BEGIN is non-reserved, so it opens a block only at statement start or as a procedure
          // body.
          if ((at_statement_start || (proc_header == ProcHeader::kAfterName && paren_depth == 0)) &&
              !BeginsTransaction(next)) {
            frames.push_back(Frame::kBlock);
            opened_block = true;
            proc_header = ProcHeader::kNone;
          }
          break;
        case Token::KW_CASE:
          frames.push_back(at_statement_start ? Frame::kBlock : Frame::kCaseExpr);
          break;
        case Token::KW_WHILE:
        case Token::KW_FOR:
          if (at_statement_start) {
            frames.push_back(Frame::kBlock);
            opened_block = true;
            awaiting_do = kind;
          }
          break;
        case Token::KW_IF:
        case Token::KW_LOOP:
        case Token::KW_REPEAT:
          if (at_statement_start) {
            frames.push_back(Frame::kBlock);
            opened_block = true;
          }
          break;
        // DO opens a loop body only after WHILE's condition or FOR's `IN (query)`; elsewhere it
        // is a name (`t AS do`, `FOR do IN`).
        // Known limit: in `WHILE do > 0 DO` the first `do` is taken for the body's DO.
        case Token::KW_DO:
          if (awaiting_do != Token::EOI && paren_depth == 0 &&
              (awaiting_do == Token::KW_WHILE || prev == Token::RPAREN)) {
            opened_loop_body = true;
            awaiting_do = Token::EOI;
          }
          break;
        default:
          break;
      }
    }

    switch (kind) {
      case Token::LPAREN:
        if (proc_header == ProcHeader::kName)
          proc_header = ProcHeader::kAfterName;
        ++paren_depth;
        break;
      case Token::KW_PROCEDURE:
        if (StartsProcedure(prev))
          proc_header = ProcHeader::kName;
        break;
      case Token::RPAREN:
        paren_depth = std::max(0, paren_depth - 1);
        break;
      case Token::KW_END:
        if (!frames.empty())
          frames.pop_back();
        break;
      case Token::SEMICOLON:
        awaiting_do = Token::EOI;
        proc_header = ProcHeader::kNone;
        paren_depth = 0;  // ';' never sits inside parentheses in valid SQL.
        // A CASE expression cannot contain ';', so any open one is unclosed.
        while (!frames.empty() && frames.back() == Frame::kCaseExpr) {
          frames.pop_back();
        }
        if (frames.empty() && token->location.start().GetByteOffset() >= error_offset) {
          return token->location.end().GetByteOffset();
        }
        break;
      default:
        break;
    }

    after_boundary = kind == Token::SEMICOLON || kind == Token::KW_THEN || kind == Token::KW_ELSE ||
                     opened_loop_body ||
                     (opened_block && (kind == Token::KW_BEGIN || kind == Token::KW_LOOP ||
                                       kind == Token::KW_REPEAT));
    prev = kind;
    token = std::move(next);
    next = std::move(after_next);
  }
  return static_cast<int>(sql.size());
}

}  // namespace googlesql::ffi
