//
// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "googlesql/ffi/googlesql_parser.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "googlesql/base/status_macros.h"
#include "googlesql/parser/parser.h"
#include "googlesql/parser/tm_token.h"
#include "googlesql/parser/token_with_location.h"
#include "googlesql/parser/tokenizer.h"
#include "googlesql/public/error_helpers.h"
#include "googlesql/public/error_location.pb.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/parse_location.h"
#include "googlesql/public/parse_resume_location.h"
#include "googlesql/public/parse_tokens.h"

namespace {

// A C++-side syntax error, before it crosses the C boundary: owns its message
struct SyntaxError {
  int start_byte;
  int end_byte;
  int line;
  int column;
  std::string message;
};

// A NUL-terminated copy of `text`, owned by the gsql_syntax_error it goes into.
const char* CopyMessage(absl::string_view text) {
  char* copy = new char[text.size() + 1];
  copy[text.copy(copy, text.size())] = '\0';
  return copy;
}

// True if nothing but whitespace and comments remains at `resume`. The parser
// reports "Unexpected end of statement" for such input, which is not what an
// editor wants to see for an empty or comment-only file.
//
// `options` drops comments and caps the scan at the first real token, so
// end-of-input means there is no statement left.
// A tokenization failure is a real error: report not-blank and let the parser
// say so.
bool AtBlankTail(const googlesql::ParseResumeLocation& resume,
                 const googlesql::ParseTokenOptions& options) {
  // We use a copy of resume here so that resume itself can be used later by the caller
  googlesql::ParseResumeLocation probe = resume;
  std::vector<googlesql::ParseToken> tokens;
  // There is at least one token and it is not the "end of file" marker
  return googlesql::GetParseTokens(options, &probe, &tokens).ok() && !tokens.empty() &&
         tokens[0].IsEndOfInput();
}

using googlesql::parser::Token;
using googlesql::parser::TokenWithLocation;

// Copied from lookahead_transformer.cc, where it is file-local.
bool IsKeyword(Token token) {
  return (token > Token::SENTINEL_RESERVED_KW_START && token < Token::SENTINEL_RESERVED_KW_END) ||
         (token > Token::SENTINEL_NONRESERVED_KW_START &&
          token < Token::SENTINEL_NONRESERVED_KW_END);
}

// What AdvanceParser found.
struct SkipStatus {
  absl::Status status;
  // The byte just past the `;` where the skip ends: the `;` that closes the statement, or, if it
  // never closes, the first `;` at or after the error. -1 if no such `;` was found.
  int skip_end = -1;
  // Skip to the end of input instead of `skip_end`: only comments follow `skip_end`, or there is
  // no `skip_end` and the scan hit end of input.
  bool should_skip_to_end_of_input = false;
  // Every block the statement opened was closed by the time the scan stopped, so the statement
  // ends there. False means the statement is unfinished (the user is likely still typing).
  bool are_all_blocks_closed = false;
  // Each statement start inside a block (the byte just past its `;`), and how many blocks are
  // open at that position.
  std::vector<std::pair<int, size_t>> open_blocks_at_statement_starts;
  // Blocks open at end of input, if the scan got there. A tokenizer error is no end of input:
  // a scan from any `;` before it reaches it too, unless it closes first.
  size_t open_blocks_at_end_of_input = std::numeric_limits<size_t>::max();
};

// Skips comments: they never affect block structure
absl::StatusOr<TokenWithLocation> NextNonCommentToken(
    googlesql::parser::GoogleSqlTokenizer& tokenizer) {
  absl::StatusOr<TokenWithLocation> token;
  do {
    token = tokenizer.GetNextToken();
  } while (token.ok() && token->kind == Token::COMMENT);
  return token;
}

// Scans the statement starting at `resume` for SkipNextStatement; see there for the arguments.
// Does not move `resume`.
SkipStatus AdvanceParser(const googlesql::ParseResumeLocation& resume,
                         int error_byte,
                         bool is_unclosed) {
  SkipStatus result;
  googlesql::parser::GoogleSqlTokenizer tokenizer(
      resume.filename(), resume.input(), resume.byte_position());

  absl::StatusOr<TokenWithLocation> next_token = NextNonCommentToken(tokenizer);

  // One entry per open block.
  // If a block is a statement we store true, otherwise (CASE expression) false,
  // to differentiate 2 types of blocks that can contain THEN/ELSE clauses.
  std::vector<bool> open_blocks;
  bool is_statement_start = true;  // a syntax error leaves resume at the statement's start
  // Between PROCEDURE and the BEGIN of its body, which is not at a statement start.
  bool is_procedure = false;
  // Between WHILE/FOR and its DO. DO elsewhere is a name (e.g. `FROM t AS do`).
  bool is_awaiting_do = false;
  // Parentheses open in the current statement: a DO inside them is a name too.
  int open_parentheses_count = 0;
  Token previous_token = Token::UNAVAILABLE;

  while (true) {
    if (!next_token.ok()) {
      result.status = next_token.status();
      return result;
    }

    // Gets the reference to TokenWithLocation inside StatusOr and copies its value to token
    const TokenWithLocation token = *next_token;
    if (token.kind == Token::EOI) {
      result.are_all_blocks_closed = open_blocks.empty();
      if (!result.are_all_blocks_closed) result.open_blocks_at_end_of_input = open_blocks.size();
      // No `;` after the error, or the statement ends here: skip to the end of input.
      if (result.are_all_blocks_closed || result.skip_end < 0)
        result.should_skip_to_end_of_input = true;
      return result;
    }
    next_token = NextNonCommentToken(tokenizer);
    const Token lookahead_token = next_token.ok() ? next_token->kind : Token::UNAVAILABLE;
    // A keyword after `.`, `@` or `@@` is an identifier.
    Token kind = token.kind;
    if ((previous_token == Token::DOT || previous_token == Token::ATSIGN ||
         previous_token == Token::KW_DOUBLE_AT) &&
        IsKeyword(kind)) {
      kind = Token::IDENTIFIER;
    }
    previous_token = kind;
    const bool was_statement_start = is_statement_start;
    // A label, as in `label: BEGIN`. Checked before keywords, as the lookahead transformer
    // does, since a non-reserved keyword such as LOOP can be a label. `e` and `e1` come from
    // the tokenizer as exponent tokens, which the transformer accepts as names too.
    if (was_statement_start && lookahead_token == Token::COLON &&
        (kind == Token::IDENTIFIER || kind == Token::EXP_IN_FLOAT_NO_SIGN ||
         kind == Token::STANDALONE_EXPONENT_SIGN || IsKeyword(kind))) {
      previous_token = Token::COLON;
      next_token = NextNonCommentToken(tokenizer);
      continue;
    }
    is_statement_start = false;
    switch (kind) {
      case Token::SEMICOLON:
        // `;` cannot appear inside an expression, so any CASE still open was left unfinished.
        while (!open_blocks.empty() && !open_blocks.back()) open_blocks.pop_back();
        is_procedure = false;
        is_awaiting_do = false;
        open_parentheses_count = 0;
        result.are_all_blocks_closed = open_blocks.empty();
        if (result.are_all_blocks_closed ||
            (result.skip_end < 0 && token.location.start().GetByteOffset() >= error_byte)) {
          result.skip_end = token.location.end().GetByteOffset();
          // Comments after the `;` are already eaten by NextNonCommentToken.
          result.should_skip_to_end_of_input = lookahead_token == Token::EOI;
          // Closed, or known never to: nothing left to scan for.
          if (result.are_all_blocks_closed || is_unclosed) return result;
        }
        result.open_blocks_at_statement_starts.emplace_back(token.location.end().GetByteOffset(),
                                                            open_blocks.size());
        is_statement_start = true;
        break;
      case Token::LPAREN:
        ++open_parentheses_count;
        break;
      case Token::RPAREN:
        --open_parentheses_count;
        break;
      case Token::KW_CASE:
        open_blocks.push_back(was_statement_start);
        break;
      case Token::KW_END:
        if (!open_blocks.empty()) open_blocks.pop_back();
        // END IF, END WHILE, etc. are one closer.
        if (lookahead_token == Token::KW_IF || lookahead_token == Token::KW_WHILE ||
            lookahead_token == Token::KW_LOOP || lookahead_token == Token::KW_REPEAT ||
            lookahead_token == Token::KW_FOR || lookahead_token == Token::KW_CASE) {
          previous_token = lookahead_token;
          next_token = NextNonCommentToken(tokenizer);
        }
        break;
      case Token::KW_IF:
      case Token::KW_WHILE:
      case Token::KW_FOR:
        if (!was_statement_start) break;  // IF(...), IF EXISTS, FOR SYSTEM_TIME, ...
        open_blocks.push_back(true);
        is_awaiting_do = kind != Token::KW_IF;
        break;
      case Token::KW_LOOP:
      case Token::KW_REPEAT:
        if (!was_statement_start) break;  // REPEAT('a', 2), ...
        open_blocks.push_back(true);
        is_statement_start = true;
        break;
      case Token::KW_BEGIN:
        // At a statement start, BEGIN followed by these is a transaction (begin_statement).
        if (is_procedure ||
            (was_statement_start && lookahead_token != Token::SEMICOLON &&
             lookahead_token != Token::EOI && lookahead_token != Token::KW_TRANSACTION &&
             lookahead_token != Token::KW_READ && lookahead_token != Token::KW_ISOLATION)) {
          is_procedure = false;
          open_blocks.push_back(true);
          is_statement_start = true;
        }
        break;
      case Token::KW_PROCEDURE:
        // PROCEDURE is non-reserved, so as a name it sets this too; harmless since only
        // BEGIN acts on it, BEGIN is reserved, and `;` resets it.
        is_procedure = true;
        break;
      case Token::KW_DO:
        if (is_awaiting_do && open_parentheses_count == 0) {
          is_awaiting_do = false;
          is_statement_start = true;
        }
        break;
      case Token::KW_THEN:
      case Token::KW_ELSE:
        is_statement_start = open_blocks.empty() || open_blocks.back();
        break;
      default:
        break;
    }
  }
}

// Script-aware version of googlesql::SkipNextStatement: moves `resume` past the statement that
// begins at its current position and failed to parse at `error_byte` (-1 if unknown), where a
// procedural block (BEGIN...END, IF...END IF, CASE...END CASE, etc.) is one statement ending at the
// `;` after its final END. Like googlesql::SkipNextStatement, it eats the comments after that `;`
// and, if nothing else follows, moves to the end of input.
//
// A block that never closes (usually the user is still typing) is skipped to the first `;` at
// or after the error instead, so the statements after it are still checked. From the error, not
// the statement start: a `;` before the error would get the error reported again. An error
// with no usable location has no better place to skip from, so it skips from the start and may
// be followed by such repeats.
//
// Block structure is read from raw tokens, mirroring googlesql.tm and the statement-start and
// script-label rules of the lookahead transformer.
//
// Returns an error, leaving `resume` and `at_end_of_input` untouched, if the input cannot be
// tokenized up to where the skip ends.
//
// `not_closed_starts` holds statement starts known not to close, so a script with many
// unclosed blocks does not scan to end of input once per error: O(n^2) on every keystroke. A scan
// from the `;` at depth d behaves like the rest of this scan with d fewer blocks open, so it closes
// at the first later `;` at depth d or less, or at end of input if the depth there is d or
// less. A scan that does not close proves it for every `;` inside it that has neither.
absl::Status SkipNextStatement(googlesql::ParseResumeLocation* resume,
                               int error_byte,
                               bool* at_end_of_input,
                               absl::flat_hash_set<int>* not_closed_starts) {
  GOOGLESQL_RETURN_IF_ERROR(resume->Validate());
  const int statement_start = resume->byte_position();
  // The caller failed to obtain the error byte before passing in here
  if (error_byte < 0) error_byte = statement_start;
  const bool is_unclosed = not_closed_starts->contains(statement_start);

  const SkipStatus result = AdvanceParser(*resume, error_byte, is_unclosed);
  // Text that cannot be tokenized after `skip_end` is left for the parser to report
  if (!result.status.ok() && result.skip_end < 0) return result.status;

  // Cache: prevents re-parsing the whole unclosed block on each statement
  // that ends with `;` inside it
  if (!result.are_all_blocks_closed && !is_unclosed) {
    size_t fewest_after = result.open_blocks_at_end_of_input;
    for (auto it = result.open_blocks_at_statement_starts.rbegin();
         it != result.open_blocks_at_statement_starts.rend();
         ++it) {
      if (fewest_after > it->second) not_closed_starts->insert(it->first);
      fewest_after = std::min(fewest_after, it->second);
    }
  }
  *at_end_of_input = result.should_skip_to_end_of_input;
  resume->set_byte_position(result.should_skip_to_end_of_input
                                ? static_cast<int>(resume->input().size())
                                : result.skip_end);
  return absl::OkStatus();
}

std::vector<SyntaxError> CheckSyntax(absl::string_view input) {
  static const absl::NoDestructor<const googlesql::LanguageOptions> language_options(
      googlesql::LanguageOptions::MaximumFeatures());
  static const googlesql::ParseTokenOptions parse_options = {
      .max_tokens = 1,
      .language_options = *language_options,
  };
  static const absl::NoDestructor<const googlesql::ParserOptions> parser_options(*language_options);

  std::vector<SyntaxError> items;
  googlesql::ParseLocationTranslator translator(input);
  googlesql::ParseResumeLocation resume = googlesql::ParseResumeLocation::FromStringView(input);

  bool at_end_of_input = false;
  int last_position = -1;
  absl::flat_hash_set<int> not_closed_starts;
  while (!at_end_of_input) {
    // Every iteration must consume input; a parser that returned without
    // advancing would spin here forever.
    const int position = resume.byte_position();
    if (position <= last_position) break;
    last_position = position;
    if (AtBlankTail(resume, parse_options)) break;

    // Script-aware: BEGIN...END, IF, WHILE, etc. each parse as one statement.
    std::unique_ptr<googlesql::ParserOutput> output;
    const absl::Status status =
        googlesql::ParseNextScriptStatement(&resume, *parser_options, &output, &at_end_of_input);
    if (status.ok()) continue;

    SyntaxError item = {-1, -1, -1, -1, std::string(status.message())};
    googlesql::ErrorLocation location;
    if (googlesql::GetErrorLocation(status, &location)) {
      item.line = location.line();
      item.column = location.column();
      item.start_byte =
          translator.GetByteOffsetFromLineAndColumn(item.line, item.column).value_or(-1);
      // ErrorLocation is a point, not a range, so the end is computed as the
      // end of the token the error points at. If it is impossible to tokenize
      // (e.g. with an unterminated string) the end is -1
      if (item.start_byte >= 0) {
        googlesql::ParseResumeLocation probe = resume;
        probe.set_byte_position(item.start_byte);
        std::vector<googlesql::ParseToken> tokens;
        if (googlesql::GetParseTokens(parse_options, &probe, &tokens).ok() && !tokens.empty()) {
          item.end_byte = tokens[0].GetLocationRange().end().GetByteOffset();
        }
      }
    }
    items.push_back(item);

    // The parser does not advance past a statement it could not parse, so skip it and
    // keep collecting. If we cannot even tokenize past here, stop and report what we have.
    if (!SkipNextStatement(&resume, item.start_byte, &at_end_of_input, &not_closed_starts).ok())
      break;
  }
  return items;
}

}  // namespace

int gsql_check_syntax(const char* sql, size_t sql_len, gsql_syntax_error** errors) {
  // The caller must not pass null pointer and the pointer must point to null,
  // because the caller cannot know beforehand how much memory to allocate
  if (errors == nullptr || *errors != nullptr) return -1;
  // A null buffer must not reach absl::string_view, and empty input parses to
  // zero errors anyway, so skip straight to that answer.
  if (sql == nullptr || sql_len == 0) return 0;
  // The parser tracks byte offsets as int.
  if (sql_len > static_cast<size_t>(std::numeric_limits<int>::max())) return -1;

  gsql_syntax_error* out = nullptr;
  int filled = 0;
  // No C++ exception (e.g. bad_alloc) may unwind into a C caller.
  try {
    const std::vector<SyntaxError> items = CheckSyntax(absl::string_view(sql, sql_len));
    if (items.empty()) return 0;
    // Handed to the caller as a plain array, so it cannot be the vector's
    // buffer. Value-initialized so a partial fill is safe to free.
    out = new gsql_syntax_error[items.size()]();
    for (const SyntaxError& item : items) {
      out[filled] = {
          item.start_byte,
          item.end_byte,
          item.line,
          item.column,
          CopyMessage(item.message),
      };
      ++filled;
    }
  } catch (...) {
    gsql_syntax_errors_free(out, filled);
    return -1;
  }
  *errors = out;
  return filled;
}

void gsql_syntax_errors_free(gsql_syntax_error* errors, int count) {
  if (errors == nullptr) return;
  for (int i = 0; i < count; ++i) delete[] errors[i].message;
  delete[] errors;
}
