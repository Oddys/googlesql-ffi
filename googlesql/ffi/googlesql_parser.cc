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

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "googlesql/public/error_helpers.h"
#include "googlesql/public/error_location.pb.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/parse_helpers.h"
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
  // We use a copy of resume here because because resume itself must not be
  // changed, otherwise it cannot be used further in
  // googlesql::IsValidNextStatementSyntax
  googlesql::ParseResumeLocation probe = resume;
  std::vector<googlesql::ParseToken> tokens;
  // There is at least one token and it is not the "end of file" marker
  return googlesql::GetParseTokens(options, &probe, &tokens).ok() &&
         !tokens.empty() && tokens[0].IsEndOfInput();
}

std::vector<SyntaxError> CheckSyntax(absl::string_view input) {
  // Built once:
  // otherwise MaximumFeatures() would walk the whole LanguageFeature enum
  // descriptor on every keystroke.
  static const absl::NoDestructor<const googlesql::LanguageOptions>
      language_options(googlesql::LanguageOptions::MaximumFeatures());
  // Subsequently, built once, too
  static const googlesql::ParseTokenOptions parse_options = {
      .max_tokens = 1, .language_options = *language_options};

  std::vector<SyntaxError> items;
  googlesql::ParseLocationTranslator translator(input);
  googlesql::ParseResumeLocation resume =
      googlesql::ParseResumeLocation::FromStringView(input);

  bool at_end_of_input = false;
  int last_position = -1;
  while (!at_end_of_input) {
    // Every iteration must consume input; a parser that returned without
    // advancing would spin here forever.
    const int position = resume.byte_position();
    if (position <= last_position) break;
    last_position = position;
    if (AtBlankTail(resume, parse_options)) break;

    const absl::Status status = googlesql::IsValidNextStatementSyntax(
        &resume, googlesql::ERROR_MESSAGE_WITH_PAYLOAD, &at_end_of_input,
        *language_options);
    if (status.ok()) continue;

    SyntaxError item = {-1, -1, -1, -1, std::string(status.message())};
    googlesql::ErrorLocation location;
    if (googlesql::GetErrorLocation(status, &location)) {
      item.line = location.line();
      item.column = location.column();
      item.start_byte =
          translator.GetByteOffsetFromLineAndColumn(item.line, item.column)
              .value_or(-1);
      // ErrorLocation is a point, not a range, so the end is computed as the
      // end of the token the error points at. If it is impossible to tokenize
      // (e.g. with an unterminated string) the end is -1
      if (item.start_byte >= 0) {
        googlesql::ParseResumeLocation probe = resume;
        probe.set_byte_position(item.start_byte);
        std::vector<googlesql::ParseToken> tokens;
        if (googlesql::GetParseTokens(parse_options, &probe, &tokens).ok() &&
            !tokens.empty()) {
          item.end_byte = tokens[0].GetLocationRange().end().GetByteOffset();
        }
      }
    }
    items.push_back(item);

    // The parser does not advance past a statement it could not parse, so skip
    // to the next semicolon and keep collecting. If we cannot even tokenize
    // past here, stop and report what we have.
    if (!googlesql::SkipNextStatement(&resume, &at_end_of_input).ok()) break;
  }
  return items;
}

}  // namespace

int gsql_check_syntax(const char* sql, size_t sql_len,
                      gsql_syntax_error** errors) {
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
    const std::vector<SyntaxError> items =
        CheckSyntax(absl::string_view(sql, sql_len));
    if (items.empty()) return 0;
    // Handed to the caller as a plain array, so it cannot be the vector's
    // buffer. Value-initialized so a partial fill is safe to free.
    out = new gsql_syntax_error[items.size()]();
    for (const SyntaxError& item : items) {
      out[filled] = {item.start_byte, item.end_byte, item.line, item.column,
                     CopyMessage(item.message)};
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
