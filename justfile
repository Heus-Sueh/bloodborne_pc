# Bloodborne PC - Cross-Platform Justfile
# Unified task runner for Linux and Windows

set shell := ["bash", "-c"]

# Show available recipes
default:
    @just --list

# Build native runtime, GPU library, and launcher
build *ARGS:
    {{ if os() == "windows" { "./scripts/run.bat --build-only " + ARGS } else { "./scripts/build.sh " + ARGS } }}

# Run comprehensive test suite
test:
    ./scripts/build.sh --test

# Run static analysis and style linting (clang-tidy & clang-format check)
lint:
    ./scripts/build.sh --lint

# Format codebase according to C/C++ coding standards
format:
    ./scripts/build.sh --format

# Launch graphical launcher (RmlUi C++20 launcher)
launcher:
    {{ if os() == "windows" { "./out/bb-launcher.exe" } else { "./launcher/bb-launcher.sh" } }}

# Run game directly
run *ARGS:
    {{ if os() == "windows" { "./out/bb-runner.exe " + ARGS } else { "./out/bb-runner " + ARGS } }}

# Extract PKG or ISO dump: just extract <file.pkg|file.iso> [output_dir]
extract FILE OUT_DIR="":
    {{ if os() == "windows" { "./tools/extractor/extract_dump.bat \"" + FILE + "\" \"" + OUT_DIR + "\"" } else { "./tools/extractor/extract_dump.sh \"" + FILE + "\" \"" + OUT_DIR + "\"" } }}

# Clean build artifacts
clean:
    rm -rf out
