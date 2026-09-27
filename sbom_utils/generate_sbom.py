#!/usr/bin/env python3
"""
generate_sbom.py: CycloneDX SBOM generator for CMake C/C++ projects.

Uses the official cyclonedx-python-lib to produce the BOM.

Two-phase approach:
  1. Static analysis:   parse CMakeLists.txt for find_package /
                        pkg_check_modules / target_link_libraries.
  2. Dynamic analysis:  drive cmake + make/ninja with verbose output, scrape
                        linker command-lines for -l<lib> flags actually used.

Usage:
    # Generate from building the binary
    python generate_sbom.py --cmake CMakeLists.txt --build --build-dir build/ --output sbom.json

    # Supply a existing verbose build log
    python generate_sbom.py --cmake CMakeLists.txt --log build/build.log --output sbom.json

Install deps:
    pip install cyclonedx-python-lib packageurl-python
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

from cyclonedx.model.bom import Bom, BomMetaData, ToolRepository
from cyclonedx.model.component import Component, ComponentType
from cyclonedx.model import Property
from cyclonedx.model.tool import Tool
from cyclonedx.output.json import JsonV1Dot5
from cyclonedx.schema import OutputFormat, SchemaVersion
from cyclonedx.validation.json import JsonStrictValidator
from packageurl import PackageURL


# Version resolution

def _resolve_version(lib_name: str) -> str | None:
    """
    Best-effort version lookup via pkg-config, then pacman / dpkg / rpm.
    Tries both bare name and lib-prefixed variant.
    """
    candidates = [lib_name, f"lib{lib_name}"]

    for name in candidates:
        try:
            out = subprocess.check_output(
                ["pkg-config", "--modversion", name],
                stderr=subprocess.DEVNULL, text=True,
            ).strip()
            if out:
                return out
        except (subprocess.CalledProcessError, FileNotFoundError):
            pass

    for mk_cmd in [
        lambda n: ["pacman", "-Q", n],
        lambda n: ["dpkg-query", "-W", "-f=${Version}", n],
        lambda n: ["rpm", "-q", "--queryformat", "%{VERSION}", n],
    ]:
        for name in candidates:
            try:
                out = subprocess.check_output(
                    mk_cmd(name), stderr=subprocess.DEVNULL, text=True,
                ).strip()
                if out and "not found" not in out.lower() and "error" not in out.lower():
                    parts = out.split()
                    return parts[1] if len(parts) > 1 else parts[0]
            except (subprocess.CalledProcessError, FileNotFoundError):
                continue

    return None


# Parsing the CMAKE
_SKIP_TOKENS = frozenset({
    "PRIVATE", "PUBLIC", "INTERFACE", "REQUIRED", "QUIET", "OPTIONAL",
    "TRUE", "FALSE", "ON", "OFF", "SHARED", "STATIC", "MODULE",
    "NAMES", "HINTS", "PATHS",
})

_CMAKE_VAR_RE    = re.compile(r'\$\{[^}]+\}')
_PKG_CHECK_RE    = re.compile(r'pkg_check_modules\s*\(\s*\w+\s+(?:REQUIRED|QUIET)?\s*([\w\-]+)', re.I)
_FIND_PKG_RE     = re.compile(r'find_package\s*\(\s*([\w\-]+)', re.I)
_FIND_LIB_RE     = re.compile(r'find_library\s*\(\s*\w+\s+NAMES\s+([\w\s\-]+?)(?:HINTS|PATHS|PATH_SUFFIXES|\))', re.I | re.S)
_TLL_ARGS_RE     = re.compile(r'target_link_libraries\s*\(\s*\w+\s+([\s\S]*?)\)', re.I)
_BARE_TOKEN_RE   = re.compile(r'\b([A-Za-z][A-Za-z0-9_\-]{1,60})\b')


def _normalize(name: str) -> str:
    """Canonical dedup key: lowercase, strip leading 'lib'."""
    return re.sub(r'^lib', '', name.lower())


def parse_cmake(path: Path, project_name: str) -> dict[str, str]:
    """
    Return {normalised_key: display_name} for all external deps in CMakeLists.txt.
    Deduplicates entries that differ only by a 'lib' prefix.
    """
    text  = path.read_text(errors="replace")
    clean = _CMAKE_VAR_RE.sub('', text)   # strip ${...} so vars don't bleed as tokens

    raw: set[str] = set()
    skip = _SKIP_TOKENS | {project_name, project_name.lower()}

    for m in _PKG_CHECK_RE.finditer(text):
        raw.add(m.group(1).strip())

    for m in _FIND_PKG_RE.finditer(text):
        pkg = m.group(1).strip()
        if pkg.lower() != "pkgconfig":
            raw.add(pkg)

    for m in _FIND_LIB_RE.finditer(text):
        for tok in m.group(1).split():
            raw.add(tok.strip())

    for m in _TLL_ARGS_RE.finditer(clean):
        for tok in _BARE_TOKEN_RE.findall(m.group(1)):
            if tok not in skip:
                raw.add(tok)

    # Deduplicate by normalised key, keep shortest display name
    canonical: dict[str, str] = {}
    for name in sorted(raw):
        if name in skip:
            continue
        key = _normalize(name)
        if not key:
            continue
        if key not in canonical or len(name) < len(canonical[key]):
            canonical[key] = name

    return canonical


# Parsing the build log

_LINK_FLAG_RE  = re.compile(r'-l([\w\-\.]+)')
_SO_PATH_RE    = re.compile(r'(?:^|[\s"])(/[^\s"]*\.so(?:\.[^\s"]*)?)')
_LINK_LINE_KWS = ("ld ", "g++ ", "gcc ", "clang++", "clang ", "link.txt", "Linking")


def scrape_link_log(log_text: str) -> set[str]:
    """Return a set of normalised library names found in a verbose build log."""
    libs: set[str] = set()
    for line in log_text.splitlines():
        if not any(kw in line for kw in _LINK_LINE_KWS):
            continue
        for m in _LINK_FLAG_RE.finditer(line):
            libs.add(_normalize(m.group(1)))
        for m in _SO_PATH_RE.finditer(line):
            stem = re.sub(r'\.so.*$', '', Path(m.group(1)).name)
            libs.add(_normalize(stem))
    libs.discard('')
    return libs


def run_cmake_build(build_dir: Path, source_dir: Path) -> str:
    """Configure + build, capturing verbose output. Returns the full log text."""
    build_dir.mkdir(parents=True, exist_ok=True)

    generator = "Unix Makefiles"
    build_cmd = ["make", "VERBOSE=1"]

    print(f"[sbom] Configuring with CMake ({generator})…", flush=True)
    cfg = subprocess.run(
        ["cmake", "-G", generator, str(source_dir.resolve())],
        cwd=build_dir, capture_output=True, text=True,
    )
    if cfg.returncode != 0:
        print("[sbom] cmake configure failed:\n", cfg.stderr, file=sys.stderr)
        sys.exit(1)

    print("[sbom] Building (verbose)…", flush=True)
    bld = subprocess.run(build_cmd, cwd=build_dir, capture_output=True, text=True)
    log = cfg.stdout + cfg.stderr + bld.stdout + bld.stderr

    log_path = build_dir / "sbom_build.log"
    log_path.write_text(log)
    print(f"[sbom] Build log saved → {log_path}", flush=True)

    if bld.returncode != 0:
        print("[sbom] Build errors encountered; SBOM will still be generated.", file=sys.stderr)

    return log


# CycloneDX BOM assembly  

def build_sbom(
    project_name: str,
    project_version: str,
    static_map: dict[str, str],   # normalised_key → display_name
    dynamic_keys: set[str],        # normalised names from build log
) -> Bom:
    all_keys   = set(static_map) | dynamic_keys
    components = []

    for key in sorted(all_keys):
        display  = static_map.get(key, key)
        version  = _resolve_version(display)

        # Track where we saw this dep
        sources = []
        if key in static_map:
            sources.append("cmake-static")
        if key in dynamic_keys:
            sources.append("build-log")

        purl = PackageURL(
            type="generic",
            name=display,
            version=version,
        )

        comp = Component(
            bom_ref=display,
            type=ComponentType.LIBRARY,
            name=display,
            version=version,
            purl=purl,
            properties=[
                Property(name="sbom:source", value=", ".join(sources)),
            ],
        )
        components.append(comp)

    # The root component (the orpheus application itself)
    root = Component(
        bom_ref=project_name,
        type=ComponentType.APPLICATION,
        name=project_name,
        version=project_version,
    )

    metadata = BomMetaData(
        component=root,
        tools=ToolRepository(tools=[
            Tool(
                vendor="generate_sbom.py",
                name="cmake-cyclonedx-sbom",
                version="1.0.0",
            )
        ]),
    )

    return Bom(components=components, metadata=metadata)


# CLI

def main() -> None:
    ap = argparse.ArgumentParser(
        description="Generate a CycloneDX SBOM for a CMake C/C++ project.",
    )
    ap.add_argument("--cmake",     default="CMakeLists.txt",
                    help="Path to CMakeLists.txt (default: ./CMakeLists.txt)")
    ap.add_argument("--output",    default="sbom.json",
                    help="Output SBOM file (default: sbom.json)")
    ap.add_argument("--project",   default=None,
                    help="Override project name (auto-detected from CMake)")
    ap.add_argument("--version",   default=None,
                    help="Override project version (auto-detected from CMake)")

    build_grp = ap.add_mutually_exclusive_group()
    build_grp.add_argument("--build", action="store_true",
                           help="Run cmake + make/ninja and capture the link log")
    build_grp.add_argument("--log",   default=None,
                           help="Path to a pre-existing verbose build log")

    ap.add_argument("--build-dir", default="build",
                    help="Build directory for --build mode (default: ./build)")
    args = ap.parse_args()

    cmake_path = Path(args.cmake)
    if not cmake_path.exists():
        ap.error(f"CMakeLists.txt not found: {cmake_path}")

    cmake_text = cmake_path.read_text(errors="replace")
    proj_m = re.search(r'project\s*\(\s*(\w+)(?:\s+VERSION\s+([\d.]+))?', cmake_text, re.I)
    project_name    = args.project or (proj_m.group(1) if proj_m else "unknown")
    project_version = args.version or (proj_m.group(2) if proj_m and proj_m.group(2) else "0.0.0")

    print(f"[sbom] Project : {project_name} {project_version}")
    print(f"[sbom] CMake   : {cmake_path}")

    # static parse
    static_map = parse_cmake(cmake_path, project_name)
    print(f"[sbom] Static deps ({len(static_map)}): {', '.join(sorted(static_map.values()))}")

    # dynamic build log
    dynamic_keys: set[str] = set()
    if args.build:
        log_text     = run_cmake_build(Path(args.build_dir), cmake_path.parent)
        dynamic_keys = scrape_link_log(log_text)
    elif args.log:
        log_path = Path(args.log)
        if not log_path.exists():
            ap.error(f"Log file not found: {log_path}")
        dynamic_keys = scrape_link_log(log_path.read_text(errors="replace"))

    if dynamic_keys:
        print(f"[sbom] Dynamic deps ({len(dynamic_keys)}): {', '.join(sorted(dynamic_keys))}")

    # Assemble and serialise via cyclonedx-python-lib
    bom = build_sbom(project_name, project_version, static_map, dynamic_keys)

    # validate 
    my_json_outputter: 'JsonOutputter' = JsonV1Dot5(bom)
    serialized_json = my_json_outputter.output_as_string(indent=2)
    my_json_validator = JsonStrictValidator(SchemaVersion.V1_7)

    try:
        json_validation_errors = my_json_validator.validate_str(serialized_json)
        if json_validation_errors:
            print('JSON invalid', 'ValidationError:', repr(json_validation_errors), sep='\n', file=sys.stderr)
            sys.exit(2)
        print('JSON valid')
    except MissingOptionalDependencyException as error:
        print('JSON-validation was skipped due to', error)

    out_path = args.output

    with open(out_path, "w") as f:
        f.write(serialized_json)

    print(f"[sbom] SBOM written → {out_path}  ({len(bom.components)} components)")


if __name__ == "__main__":
    main()
