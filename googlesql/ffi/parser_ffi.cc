#include "googlesql/ffi/parser_ffi.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "googlesql/ffi/statement_skipper.h"
#include "googlesql/parser/parser.h"
#include "googlesql/public/error_helpers.h"
#include "googlesql/public/error_location.pb.h"
#include "googlesql/public/language_options.h"
#include "googlesql/public/options.pb.h"
#include "googlesql/public/parse_location.h"
#include "googlesql/public/parse_resume_location.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace {

using ::googlesql::ffi::OnlyTriviaFrom;
using ::googlesql::ffi::SkipStatement;
using ::googlesql::ffi::TokenRangeAt;

struct Found {
  int start;
  int end;
  std::string message;
};

googlesql::ParserOptions MakeParserOptions() {
  googlesql::LanguageOptions language_options;
  language_options.EnableMaximumLanguageFeatures();
  language_options.SetSupportsAllStatementKinds();
  return googlesql::ParserOptions(
      /*id_string_pool=*/nullptr, /*arena=*/nullptr, language_options,
      {.mode = googlesql::ERROR_MESSAGE_WITH_PAYLOAD,
       .attach_error_location_payload = true},
      googlesql::parser::MacroExpansionMode::kNone, /*macro_catalog=*/nullptr);
}

// Byte offset of the error in `status`, or `fallback` if it has none.
int ErrorOffset(const absl::Status& status,
                const googlesql::ParseLocationTranslator& translator,
                int fallback) {
  googlesql::ErrorLocation location;
  if (!googlesql::GetErrorLocation(status, &location)) return fallback;
  absl::StatusOr<int> offset = translator.GetByteOffsetFromLineAndColumn(
      location.line(), location.column());
  return offset.ok() ? *offset : fallback;
}

// Appends one Found per syntax error. Returns false on internal failure.
bool CollectDiagnostics(absl::string_view sql, std::vector<Found>& found) {
  if (OnlyTriviaFrom(sql, 0)) return true;
  const googlesql::ParserOptions options = MakeParserOptions();
  const googlesql::ParseLocationTranslator translator(sql);
  googlesql::ParseResumeLocation location =
      googlesql::ParseResumeLocation::FromStringView(sql);
  bool at_end = false;
  while (!at_end) {
    const int stmt_start = location.byte_position();
    std::unique_ptr<googlesql::ParserOutput> output;
    absl::Status status = googlesql::ParseNextScriptStatement(
        &location, options, &output, &at_end);
    if (status.ok()) continue;
    if (!absl::IsInvalidArgument(status)) return false;
    const int error_offset = ErrorOffset(status, translator, stmt_start);
    auto [start, end] = TokenRangeAt(sql, error_offset);
    found.push_back({start, end, std::string(status.message())});
    int next = SkipStatement(sql, stmt_start, error_offset);
    if (next <= stmt_start) next = static_cast<int>(sql.size());  // progress
    location.set_byte_position(next);
    at_end = OnlyTriviaFrom(sql, next);
  }
  return true;
}

}  // namespace

extern "C" int gsql_parse_script(const char* sql, size_t len,
                                 gsql_diagnostics* out) {
  if (out == nullptr) return 1;
  *out = {nullptr, 0};
  if (len > static_cast<size_t>(INT32_MAX) || (sql == nullptr && len != 0)) {
    return 1;
  }
  std::vector<Found> found;
  if (!CollectDiagnostics(absl::string_view(sql, len), found)) return 1;
  if (found.empty()) return 0;
  out->items = new gsql_diagnostic[found.size()];
  for (size_t i = 0; i < found.size(); ++i) {
    const std::string& text = found[i].message;
    char* message = new char[text.size() + 1];
    std::memcpy(message, text.c_str(), text.size() + 1);
    out->items[i] = {static_cast<uint32_t>(found[i].start),
                     static_cast<uint32_t>(found[i].end), message};
  }
  out->count = found.size();
  return 0;
}

extern "C" void gsql_diagnostics_free(gsql_diagnostics* out) {
  if (out == nullptr) return;
  for (size_t i = 0; i < out->count; ++i) delete[] out->items[i].message;
  delete[] out->items;
  *out = {nullptr, 0};
}
