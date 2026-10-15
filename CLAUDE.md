# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

GoogleSQL (formerly ZetaSQL) is a SQL language front end: grammar, parser, analyzer (name resolution, type checking, coercion), function library, and a reference execution engine. It is not a database. Upstream is a one-way export from Google ("Export of internal GoogleSQL changes" commits); there is no API stability guarantee.

## Build & test

Bazel (version pinned in `.bazelversion`, 7.x, bzlmod via `MODULE.bazel`). C++20 and clang settings come from `.bazelrc` automatically; macOS also gets `--config=macos` (layering_check disabled). Requires `tzdata`. Full builds are very slow — build/test specific targets.

```bash
bazel build //googlesql/public:analyzer                    # build one target
bazel test //googlesql/parser:fast                         # fast parser tests
bazel test //googlesql/parser:parser_<file>_test           # one parser .test file, e.g. parser_set_test
bazel test //googlesql/analyzer:analyzer_<file>_test       # one analyzer .test file, e.g. analyzer_aggregation_test
bazel test //googlesql/public:cast_test                    # ordinary gtest
bazel test //some:test --test_filter='Suite.Case'          # single gtest case
bazel run //googlesql/tools/execute_query:execute_query -- --web   # interactive parse/analyze/run (see execute_query.md)
```

Python code generators run on a downloaded Python 3.11 from `rules_python` (no venv). A system `python3` is needed for the `py_binary` launcher stub that starts with `#!/usr/bin/env python3` and then execs the downloaded interpreter. Python protobuf comes from the Bazel `protobuf` module, and other pip packages come from `requirements.txt` (`@pypi`). The README's `pip install protobuf` / `googlesql_deps_step_2.bzl` tip is stale. A `Dockerfile` / `docker_build.sh` provide a reference Ubuntu build environment.

## File-based tests

Most parser and analyzer coverage lives in golden `.test` files driven by `file_based_test_driver`, not in gtest code:

- `googlesql/parser/testdata/*.test` → one `parser_<name>_test` target per file (macro `gen_parser_test` in `parser/builddefs.bzl`).
- `googlesql/analyzer/testdata/*.test` → one `analyzer_<name>_test` per file (`gen_analyzer_test` in `analyzer/builddefs.bzl`), run against the sample catalog in `googlesql/testdata/sample_catalog.*`.
- `googlesql/compliance/testdata/*.test` → compliance tests run against the reference implementation.

Format: `[default option=...]` headers, then cases of `SQL` followed by `--` and the expected output (parse tree / Resolved AST / error), cases separated by `==`. `{{a|b}}` creates alternations. Options such as `language_features=...` toggle `LanguageFeature` flags (`googlesql/public/options.proto`). When behavior changes, the expected output in these files must be updated to match.

## Architecture

Pipeline: **SQL text → parser → AST (`ASTNode`) → analyzer/resolver → Resolved AST → (optional rewriters) → engine** (the reference impl in this repo, or an external engine).

- `googlesql/public/` — public API: `analyzer.h` (`AnalyzeStatement`, etc.), `AnalyzerOptions`/`LanguageOptions`, `Catalog`/`SimpleCatalog`, `types/`, `Value`, `functions/` (function implementations for engines), `evaluator.h` (`PreparedExpression`/`PreparedQuery` over the reference impl).
- `googlesql/parser/` — grammar is `googlesql.tm` (Textmapper, rules in `bazel/textmapper.bzl`). AST node classes are **code-generated** by `gen_parse_tree.py` from `*.template` files; edit the generator, not generated output. Parse trees are semi-public (not a stable API). `unparser` turns AST back to SQL; `macros/` handles macro expansion.
- `googlesql/analyzer/` — `Resolver`, split across `resolver_*.cc` (query, expr, stmt, dml, alter...); `rewriters/` transform the Resolved AST (anonymization, flatten, inlining, etc.) controlled by `AnalyzerOptions` enabled rewrites.
- `googlesql/resolved_ast/` — Resolved AST node classes, visitors, deep-copy, comparator, proto, and `docs/resolved_ast.md` are all **code-generated** by `gen_resolved_ast.py` (node definitions live in its `main()`) from `*.template` files.
- `googlesql/common/` — internal shared code; builtin function signatures are registered in `builtin_function_internal_*.cc`.
- `googlesql/reference_impl/` — reference engine: `algebrizer` converts Resolved AST to an operator tree, `evaluation` runs it; `reference_driver` hooks it into compliance tests.
- `googlesql/compliance/` — compliance framework and function test libs (`functions_testlib_*.cc`) used to validate engines.
- `googlesql/local_service/` + `java/` — Java API implemented over `local_service.proto`, a service wrapping the C++ library and exposed via gRPC (`local_service_grpc`) or JNI (`local_service_jni`).
- `docs/` — user-facing SQL language reference (markdown).
