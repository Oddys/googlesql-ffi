#include "googlesql/ffi/statement_skipper.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "googlesql/parser/tm_token.h"
#include "googlesql/parser/token_with_location.h"
#include "googlesql/parser/tokenizer.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace googlesql::ffi {
namespace {

using ::googlesql::parser::GoogleSqlTokenizer;
using ::googlesql::parser::Token;
using ::googlesql::parser::TokenWithLocation;

// Next non-comment token, or nullopt if the tokenizer fails.
std::optional<TokenWithLocation> NextSignificant(GoogleSqlTokenizer& tokenizer) {
  while (true) {
    absl::StatusOr<TokenWithLocation> token = tokenizer.GetNextToken();
    if (!token.ok()) return std::nullopt;
    if (token->kind != Token::COMMENT) return *token;
  }
}

// BEGIN followed by these is BEGIN [TRANSACTION] [modes], not a block.
bool BeginsTransaction(const std::optional<TokenWithLocation>& next) {
  if (!next.has_value()) return true;
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
    if (eol == absl::string_view::npos) eol = sql.size();
    return {offset, static_cast<int>(eol)};
  }
  if (token->kind == Token::EOI) {
    return {static_cast<int>(sql.size()), static_cast<int>(sql.size())};
  }
  return {token->location.start().GetByteOffset(),
          token->location.end().GetByteOffset()};
}

bool OnlyTriviaFrom(absl::string_view sql, int offset) {
  GoogleSqlTokenizer tokenizer(/*filename=*/"", sql, offset);
  std::optional<TokenWithLocation> token = NextSignificant(tokenizer);
  return token.has_value() && token->kind == Token::EOI;
}

int SkipStatement(absl::string_view sql, int stmt_start, int error_offset) {
  enum class Frame { kBlock, kCaseExpr };
  std::vector<Frame> frames;
  int paren_depth = 0;
  // True when the previous token lets a new statement begin here.
  bool after_boundary = true;
  Token prev = Token::EOI;  // EOI stands for "no previous token".

  GoogleSqlTokenizer tokenizer(/*filename=*/"", sql, stmt_start);
  std::optional<TokenWithLocation> token = NextSignificant(tokenizer);
  while (token.has_value() && token->kind != Token::EOI) {
    std::optional<TokenWithLocation> next = NextSignificant(tokenizer);
    const Token kind = token->kind;
    const bool at_statement_start =
        after_boundary && paren_depth == 0 &&
        (frames.empty() || frames.back() != Frame::kCaseExpr);
    bool opened_block = false;

    // A keyword right after END closes (END IF, END CASE, ...); never opens.
    if (prev != Token::KW_END) {
      switch (kind) {
        case Token::KW_BEGIN:
          // BEGIN is non-reserved: only a block at statement start or as a
          // procedure body after ')'.
          if ((at_statement_start || prev == Token::RPAREN) &&
              !BeginsTransaction(next)) {
            frames.push_back(Frame::kBlock);
            opened_block = true;
          }
          break;
        case Token::KW_CASE:
          frames.push_back(at_statement_start ? Frame::kBlock
                                              : Frame::kCaseExpr);
          break;
        case Token::KW_IF:
        case Token::KW_LOOP:
        case Token::KW_WHILE:
        case Token::KW_REPEAT:
        case Token::KW_FOR:
          if (at_statement_start) {
            frames.push_back(Frame::kBlock);
            opened_block = true;
          }
          break;
        default:
          break;
      }
    }

    switch (kind) {
      case Token::LPAREN:
        ++paren_depth;
        break;
      case Token::RPAREN:
        paren_depth = std::max(0, paren_depth - 1);
        break;
      case Token::KW_END:
        if (!frames.empty()) frames.pop_back();
        break;
      case Token::SEMICOLON:
        paren_depth = 0;  // ';' never sits inside parentheses in valid SQL.
        if (frames.empty() &&
            token->location.start().GetByteOffset() >= error_offset) {
          return token->location.end().GetByteOffset();
        }
        break;
      default:
        break;
    }

    after_boundary =
        kind == Token::SEMICOLON || kind == Token::KW_THEN ||
        kind == Token::KW_ELSE || kind == Token::KW_DO ||
        kind == Token::COLON ||
        (opened_block && (kind == Token::KW_BEGIN || kind == Token::KW_LOOP ||
                          kind == Token::KW_REPEAT));
    prev = kind;
    token = std::move(next);
  }
  return static_cast<int>(sql.size());
}

}  // namespace googlesql::ffi
