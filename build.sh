#!/usr/bin/env bash
# Global build+run for AZBacktest. Callable from anywhere in the project.
#
# Usage:
#   build.sh -runfile <path>
#     <path> is resolved relative to the current directory (where you invoked the script).
#   build.sh -tests
#     Build and run every tests/test_*.cpp, one binary each, and report a summary.
#   build.sh -clean
#     Sweep leftover .build_*.exe artifacts from interrupted runs across the project.
#   build.sh -zip
#     Run the amalgamation, then zip the resulting azbacktest/ folder into azbacktest.zip.
#
# Behavior:
#   - Auto-discovers sibling .cpp files matching #include "X.h" directives and links them.
#   - Adds the project root to the include path so dataConfig.h is available anywhere.
#   - Runs the binary from the target file's own directory so its relative paths work.
#   - Deletes the built executable when done.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"
INVOKE_DIR="$(pwd)"

RUNFILE=""
CLEAN=0
TESTS=0
ZIP=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        -runfile)
            RUNFILE="$2"
            shift 2
            ;;
        -tests)
            TESTS=1
            shift
            ;;
        -clean)
            CLEAN=1
            shift
            ;;
        -zip)
            ZIP=1
            shift
            ;;
        *)
            echo "unknown arg: $1" >&2
            echo "usage: build.sh -runfile <path> | -tests | -clean | -zip" >&2
            exit 1
            ;;
    esac
done

if [[ $CLEAN -eq 1 ]]; then
    count=0
    while IFS= read -r -d '' f; do
        rm -f "$f"
        count=$((count + 1))
    done < <(find "$PROJECT_ROOT" \( -name '.build_*.exe' -o -name '.azt_test_*.csv' \) -type f -print0)
    echo "removed $count build artifact(s)"
    exit 0
fi

# Build and run the unit tests. Each tests/test_*.cpp becomes its own binary
# rather than one linked suite, because backtestApi.h declares `trades`,
# `realizedProfit` and `equityCurve` as non-inline globals - two test TUs that
# both include it would collide at link time. Separate binaries also keep the
# global kCSVMapping from leaking between files.
#
# CSV-only: the fixtures are generated CSVs, so nothing here needs Arrow and the
# Parquet backend isn't exercised.
if [[ $TESTS -eq 1 ]]; then
    TEST_DIR="$PROJECT_ROOT/tests"
    if [[ ! -d "$TEST_DIR" ]]; then
        echo "no tests/ directory at $TEST_DIR" >&2
        exit 1
    fi

    shopt -s nullglob
    TEST_FILES=("$TEST_DIR"/test_*.cpp)
    shopt -u nullglob
    if [[ ${#TEST_FILES[@]} -eq 0 ]]; then
        echo "no test files matching tests/test_*.cpp" >&2
        exit 1
    fi

    cd "$PROJECT_ROOT" # fixtures are written relative to the working directory

    suites_run=0
    suites_failed=0
    failed_names=()
    for tf in "${TEST_FILES[@]}"; do
        suite="$(basename "$tf" .cpp)"
        exe="$PROJECT_ROOT/.build_test_${suite}_$$.exe"

        # set -e is on, so guard the compile explicitly to report which suite
        # broke instead of dying with a bare g++ error
        if ! g++ -std=c++17 -I"$PROJECT_ROOT" -DAZT_SUITE_NAME="\"$suite\"" \
                 "$tf" -o "$exe"; then
            echo "  BUILD FAILED  $suite"
            echo ""
            suites_failed=$((suites_failed + 1))
            failed_names+=("$suite (build)")
            suites_run=$((suites_run + 1))
            continue
        fi

        set +e
        "$exe"
        status=$?
        set -e

        rm -f "$exe"
        suites_run=$((suites_run + 1))
        if [[ $status -ne 0 ]]; then
            suites_failed=$((suites_failed + 1))
            failed_names+=("$suite")
        fi
    done

    # sweep any fixture a crashed test didn't get to clean up itself
    rm -f "$PROJECT_ROOT"/.azt_test_*.csv

    if [[ $suites_failed -eq 0 ]]; then
        echo "all $suites_run suite(s) passed"
        exit 0
    fi
    echo "$suites_failed of $suites_run suite(s) failed:"
    for n in "${failed_names[@]}"; do echo "  - $n"; done
    exit 1
fi

if [[ $ZIP -eq 1 ]]; then
    bash "$SCRIPT_DIR/amalgamate.sh"
    ZIP_PATH="$PROJECT_ROOT/azbacktest.zip"
    rm -f "$ZIP_PATH"
    if command -v zip >/dev/null 2>&1; then
        (cd "$PROJECT_ROOT" && zip -rq "$ZIP_PATH" azbacktest)
    else
        # git-bash/MSYS doesn't ship zip by default, fall back to PowerShell
        WIN_SRC="$(cygpath -w "$PROJECT_ROOT/azbacktest")"
        WIN_DST="$(cygpath -w "$ZIP_PATH")"
        powershell.exe -NoProfile -Command "Compress-Archive -Path '$WIN_SRC' -DestinationPath '$WIN_DST' -Force"
    fi
    echo "wrote $ZIP_PATH"
    exit 0
fi

if [[ -z "$RUNFILE" ]] && [[ $CLEAN -eq 0 ]]; then
    bash "$SCRIPT_DIR/amalgamate.sh"
    exit 0
fi

# Resolve to absolute path, relative to invocation directory.
if [[ "$RUNFILE" = /* ]] || [[ "$RUNFILE" =~ ^[a-zA-Z]: ]]; then
    ABS_FILE="$RUNFILE"
else
    ABS_FILE="$INVOKE_DIR/$RUNFILE"
fi

if [[ ! -f "$ABS_FILE" ]]; then
    echo "file not found: $ABS_FILE" >&2
    exit 1
fi

FILE_DIR="$(cd "$(dirname "$ABS_FILE")" && pwd)"
DEPS=()

SCANNED=()
collect_deps() {
    local file="$1"
    local dir
    dir="$(cd "$(dirname "$file")" && pwd)"

    for s in "${SCANNED[@]}"; do
        if [[ "$s" = "$file" ]]; then return; fi
    done
    SCANNED+=("$file")

    while IFS= read -r line; do
        if [[ "$line" =~ ^[[:space:]]*#include[[:space:]]*\"([^\"]+)\" ]]; then
            local inc="${BASH_REMATCH[1]}"
            local inc_path="$dir/$inc"
            if [[ -f "$inc_path" ]]; then
                collect_deps "$inc_path"
            fi
            local cpp_path
            cpp_path="$(cd "$(dirname "${inc_path%.h}.cpp")" 2>/dev/null && echo "$(pwd)/$(basename "${inc_path%.h}.cpp")" || true)"
            if [[ -f "$cpp_path" ]] && [[ "$cpp_path" != "$ABS_FILE" ]]; then
                local already=0
                for d in "${DEPS[@]}"; do
                    if [[ "$d" = "$cpp_path" ]]; then already=1; break; fi
                done
                if [[ $already -eq 0 ]]; then
                    DEPS+=("$cpp_path")
                    collect_deps "$cpp_path"
                fi
            fi
        fi
    done < "$file"
}

collect_deps "$ABS_FILE"

# Detect vendored ImGui usage anywhere in the target or its deps.
EXTRA_INCLUDES=()
EXTRA_SOURCES=()
EXTRA_LIBS=()
EXTRA_DEFINES=()
uses_imgui=0
for f in "${SCANNED[@]}"; do
    if grep -qE '^\s*#\s*include\s*[<"]imgui\.h[>"]' "$f"; then
        uses_imgui=1; break
    fi
done
if [[ $uses_imgui -eq 1 ]]; then
    EXTRA_INCLUDES+=(-I"$PROJECT_ROOT/vendor/imgui" -I"$PROJECT_ROOT/vendor/imgui/backends")
    EXTRA_SOURCES+=(
        "$PROJECT_ROOT/vendor/imgui/imgui.cpp"
        "$PROJECT_ROOT/vendor/imgui/imgui_draw.cpp"
        "$PROJECT_ROOT/vendor/imgui/imgui_tables.cpp"
        "$PROJECT_ROOT/vendor/imgui/imgui_widgets.cpp"
        "$PROJECT_ROOT/vendor/imgui/backends/imgui_impl_glfw.cpp"
        "$PROJECT_ROOT/vendor/imgui/backends/imgui_impl_opengl3.cpp"
    )
    # Platform-specific libraries and includes
    if [[ "$OSTYPE" == "darwin"* ]]; then
        # macOS - use Homebrew GLFW (detect correct prefix, with fallback)
        if [[ -f "/opt/homebrew/lib/libglfw.dylib" ]] || [[ -f "/opt/homebrew/lib/libglfw3.a" ]]; then
            BREW_PREFIX="/opt/homebrew"
        elif [[ -f "/usr/local/lib/libglfw.dylib" ]] || [[ -f "/usr/local/lib/libglfw3.a" ]]; then
            BREW_PREFIX="/usr/local"
        else
            echo "Error: GLFW not found. Install with: brew install glfw" >&2
            exit 1
        fi
        EXTRA_INCLUDES+=(-I"$BREW_PREFIX/include")
        EXTRA_LIBS+=(-L"$BREW_PREFIX/lib" -lglfw -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo)
    else
        # Windows (assumes MinGW/MSYS) - use vendored GLFW
        EXTRA_INCLUDES+=(-I"$PROJECT_ROOT/vendor/glfw/include")
        EXTRA_LIBS+=(-L"$PROJECT_ROOT/vendor/glfw/lib" -lglfw3 -lopengl32 -lgdi32 -lshell32 -lwinmm)
    fi
fi

uses_implot=0
for f in "${SCANNED[@]}"; do
    if grep -qE '^\s*#\s*include\s*[<"]implot\.h[>"]' "$f"; then
        uses_implot=1; break
    fi
done
if [[ $uses_implot -eq 1 ]]; then
    EXTRA_INCLUDES+=(-I"$PROJECT_ROOT/vendor/implot")
    EXTRA_SOURCES+=(
        "$PROJECT_ROOT/vendor/implot/implot.cpp"
        "$PROJECT_ROOT/vendor/implot/implot_items.cpp"
    )
fi

# Eigen is header-only and vendored, so just add its include path when used.
uses_eigen=0
for f in "${SCANNED[@]}"; do
    if grep -qE '^\s*#\s*include\s*[<"]Eigen/' "$f"; then
        uses_eigen=1; break
    fi
done
if [[ $uses_eigen -eq 1 ]]; then
    EXTRA_INCLUDES+=(-I"$PROJECT_ROOT/vendor/eigen")
fi

# matplot++ isn't header-only, vendor/matplot holds its headers plus a MinGW
# prebuilt libmatplot.a/libnodesoup.a (v1.2.1, same idea as glfw). it shells
# out to gnuplot at runtime so that needs to be on PATH
uses_matplot=0
for f in "${SCANNED[@]}"; do
    if grep -qE '^\s*#\s*include\s*[<"]matplot/' "$f"; then
        uses_matplot=1; break
    fi
done
if [[ $uses_matplot -eq 1 ]]; then
    if [[ "$OSTYPE" == "darwin"* ]]; then
        if [[ -f "/opt/homebrew/lib/libmatplot.a" ]] || [[ -f "/opt/homebrew/lib/libmatplot.dylib" ]]; then
            MPP_PREFIX="/opt/homebrew"
        elif [[ -f "/usr/local/lib/libmatplot.a" ]] || [[ -f "/usr/local/lib/libmatplot.dylib" ]]; then
            MPP_PREFIX="/usr/local"
        else
            echo "Error: matplot++ not found. Install with: brew install matplotplusplus" >&2
            exit 1
        fi
        EXTRA_INCLUDES+=(-I"$MPP_PREFIX/include")
        EXTRA_LIBS+=(-L"$MPP_PREFIX/lib" -L"$MPP_PREFIX/lib/Matplot++" -lmatplot -lnodesoup)
    else
        EXTRA_INCLUDES+=(-I"$PROJECT_ROOT/vendor/matplot/include")
        EXTRA_DEFINES+=(-DNOMINMAX)
        EXTRA_LIBS+=(-L"$PROJECT_ROOT/vendor/matplot/lib" -lmatplot -lnodesoup -lgdi32)
    fi
fi

# marketData.h always carries the Parquet backend behind #ifdef AZBT_PARQUET
# (both in the src/ tree and inlined into the amalgamated azbacktest.h), so
# detect it by grepping for that macro rather than a specific #include - it
# needs to fire whether the strategy includes marketData.h/backtestApi.h
# directly or just the single-header release build.
uses_parquet=0
for f in "${SCANNED[@]}"; do
    if grep -q 'AZBT_PARQUET' "$f" 2>/dev/null; then
        uses_parquet=1; break
    fi
done
CXX_STD="c++17"
if [[ $uses_parquet -eq 1 ]]; then
    if [[ "$OSTYPE" == "darwin"* ]]; then
        if [[ -f "/opt/homebrew/lib/libarrow.dylib" ]] && [[ -f "/opt/homebrew/lib/libparquet.dylib" ]]; then
            ARROW_PREFIX="/opt/homebrew"
        elif [[ -f "/usr/local/lib/libarrow.dylib" ]] && [[ -f "/usr/local/lib/libparquet.dylib" ]]; then
            ARROW_PREFIX="/usr/local"
        fi
        if [[ -n "${ARROW_PREFIX:-}" ]]; then
            EXTRA_INCLUDES+=(-I"$ARROW_PREFIX/include")
            EXTRA_LIBS+=(-L"$ARROW_PREFIX/lib" -larrow -lparquet)
            EXTRA_DEFINES+=(-DAZBT_PARQUET)
            # Arrow's headers use C++20 library features (std::span,
            # std::popcount); bump the standard only when it's actually
            # linked in so CSV-only builds without Arrow are unaffected.
            CXX_STD="c++20"
        else
            echo "Note: building without Parquet support (Arrow not found). .parquet data files won't work until you: brew install apache-arrow" >&2
        fi
    else
        # Windows (assumes MinGW/MSYS, matching the GLFW branch above). Arrow
        # is too heavy to vendor like GLFW (its own dependency graph pulls in
        # llvm/grpc/aws-sdk-cpp/etc), so look for a vcpkg install instead -
        # specifically a MinGW triplet, since vcpkg's default MSVC-built libs
        # aren't ABI-compatible with g++:
        #   vcpkg install arrow[parquet]:x64-mingw-dynamic
        #
        # UNTESTED on Windows - this is a best-effort guess at vcpkg's layout
        # and triplet naming. If it doesn't find your install, check the
        # candidate paths/triplets below against your actual
        # <vcpkg root>/installed/<triplet> directory.
        VCPKG_ARROW_ROOT=""
        VCPKG_CANDIDATES=("$PROJECT_ROOT/vcpkg" "/c/vcpkg" "/c/tools/vcpkg")
        if [[ -n "${VCPKG_ROOT:-}" ]]; then
            NORMALIZED_VCPKG_ROOT="$VCPKG_ROOT"
            if command -v cygpath >/dev/null 2>&1; then
                NORMALIZED_VCPKG_ROOT="$(cygpath -u "$VCPKG_ROOT" 2>/dev/null || echo "$VCPKG_ROOT")"
            fi
            VCPKG_CANDIDATES=("$NORMALIZED_VCPKG_ROOT" "${VCPKG_CANDIDATES[@]}")
        fi
        for VCPKG_CANDIDATE in "${VCPKG_CANDIDATES[@]}"; do
            # dynamic first: linking just -larrow -lparquet against a static
            # triplet also needs every transitive dep (thrift/snappy/zstd/...)
            # spelled out explicitly, which the dynamic triplet avoids
            for TRIPLET in x64-mingw-dynamic x64-mingw-static; do
                if [[ -f "$VCPKG_CANDIDATE/installed/$TRIPLET/include/arrow/api.h" ]]; then
                    VCPKG_ARROW_ROOT="$VCPKG_CANDIDATE/installed/$TRIPLET"
                    break 2
                fi
            done
        done

        if [[ -n "$VCPKG_ARROW_ROOT" ]]; then
            EXTRA_INCLUDES+=(-I"$VCPKG_ARROW_ROOT/include")
            EXTRA_LIBS+=(-L"$VCPKG_ARROW_ROOT/lib" -larrow -lparquet)
            EXTRA_DEFINES+=(-DAZBT_PARQUET)
            CXX_STD="c++20"
        else
            echo "Note: building without Parquet support (no vcpkg Arrow install found)." >&2
            echo "  Install with: vcpkg install arrow[parquet]:x64-mingw-dynamic" >&2
            echo "  Then either set VCPKG_ROOT, or install vcpkg at C:\\vcpkg." >&2
        fi
    fi
fi

OUT_EXE="$FILE_DIR/.build_$$.exe"
g++ -std="$CXX_STD" -I"$PROJECT_ROOT" "${EXTRA_DEFINES[@]}" "${EXTRA_INCLUDES[@]}" "$ABS_FILE" "${DEPS[@]}" "${EXTRA_SOURCES[@]}" -o "$OUT_EXE" "${EXTRA_LIBS[@]}"

cd "$PROJECT_ROOT"
"$OUT_EXE"
STATUS=$?

rm -f "$OUT_EXE"
exit $STATUS
