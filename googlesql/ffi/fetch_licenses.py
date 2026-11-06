#!/usr/bin/env python3
"""Downloads dependency licenses to LICENSES/ and stores metadata to licenses-metadata.json.

Versions come from MODULE.bazel.lock, bazel/http_archive_deps.bzl (ICU) and vcpkg.json (ICU on Windows).
The list of linked dependencies comes from check_linked.sh.
"""

import json
import re
import subprocess
from pathlib import Path

FFI_DIR = Path(__file__).resolve().parent
ROOT = FFI_DIR.parents[1]
LICENSES_DIR = FFI_DIR / "LICENSES"

# Dependency: (Bazel module that provides it, license path in its repository).
DEPS = {
    "abseil-cpp": ("abseil-cpp", "LICENSE"),
    "googleapis": ("googleapis", "LICENSE"),
    "googletest": ("googletest", "LICENSE"),
    "icu": (None, "LICENSE"),
    "icu-windows": ("vcpkg", "LICENSE"),
    "nlohmann_json": ("nlohmann_json", "LICENSE.MIT"),
    "protobuf": ("protobuf", "LICENSE"),
    "utf8_range": ("protobuf", "third_party/utf8_range/LICENSE"),
    "re2": ("re2", "LICENSE"),
    "zlib": ("zlib", "LICENSE"),
}

GITHUB_ARCHIVE = re.compile(
    r"github\.com/([^/]+/[^/]+)/"
    r"(?:releases/download/([^/]+)/|archive/(?:refs/tags/)?(.+?)\.(?:tar\.gz|zip)$)"
)


def fetch(url):
    return subprocess.run(
        ["curl", "-fsSL", url], check=True, capture_output=True
    ).stdout


def archive_url(module, lock):
    """Returns the source archive URL of `module`, or of ICU if it is None or "vcpkg"."""
    if module == "vcpkg":
        vcpkg = json.loads((ROOT / "vcpkg.json").read_text())
        version = next(o["version"] for o in vcpkg["overrides"] if o["name"] == "icu")
        return f"https://github.com/unicode-org/icu/archive/refs/tags/release-{version}.tar.gz"
    if module is None:
        bzl = (ROOT / "bazel/http_archive_deps.bzl").read_text()
        return re.search(r'"(https://\S+/icu4c-\S+)"', bzl)[1]
    source = re.search(rf'https://[^"]*/modules/{module}/[^/]+/source\.json', lock)
    return json.loads(fetch(source[0]))["url"]


def main():
    lock = (ROOT / "MODULE.bazel.lock").read_text()
    metadata = []
    for dep, (module, license_path) in DEPS.items():
        match = GITHUB_ARCHIVE.search(archive_url(module, lock))
        repo, ref = match[1], match[2] or match[3]
        saved_as = Path(dep) / Path(license_path).name
        url = f"https://raw.githubusercontent.com/{repo}/{ref}/{license_path}"
        content = fetch(url)
        (LICENSES_DIR / dep).mkdir(parents=True, exist_ok=True)
        (LICENSES_DIR / saved_as).write_bytes(content)
        metadata.append(
            {
                "dependency": dep,
                "fetched_from": url,
                "saved_as": str(saved_as),
            }
        )
    metadata_path = LICENSES_DIR / "licenses-metadata.json"
    metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")


if __name__ == "__main__":
    main()
