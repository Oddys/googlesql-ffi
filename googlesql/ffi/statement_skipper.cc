#include "googlesql/ffi/statement_skipper.h"

#include <optional>
#include <utility>

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

}  // namespace googlesql::ffi
