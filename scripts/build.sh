#!/usr/bin/env bash
# Configures and builds wvt for one CPU architecture (issue #71).
#
#   scripts/build.sh [options] [-- extra CMake arguments]
#
# The build folder is build.<arch> at the top of the source tree (for
# example build.arm64 or build.x86_64), or build.<arch>.<sanitizer> for a
# sanitizer build. Run scripts/build.sh --help for the options.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)

usage() {
    cat <<'EOF'
usage: scripts/build.sh [options] [-- extra CMake arguments]

Configures and builds wvt in build.<arch> (for example build.arm64).

architecture and build:
  -a, --arch ARCH        x86_64 or arm64 (also amd64, aarch64); default: this computer's
  -t, --type TYPE        Release (default), Debug, RelWithDebInfo or MinSizeRel
  -j, --jobs N           parallel compile jobs (default: all CPUs)
  -B, --build-dir DIR    use DIR instead of build.<arch>
      --toolchain FILE   CMake toolchain file (Linux: needed for another architecture)
      --clean            delete the build folder first
      --test             run the self-checks (ctest) after the build
  -n, --dry-run          show the commands, do not run them
  -h, --help             show this help

features (default: each one is on if its library is found):
      --no-rtlsdr        no RTL-SDR backend         (--with-rtlsdr: stop if it is missing)
      --no-uhd           no USRP backend (UHD)      (--with-uhd)
      --no-bb            no Signal Hound BB60       (--with-bb)
      --no-audio         no audio playback          (--with-audio)
      --no-cuda          no CUDA GPU backend        (--with-cuda)
      --no-metal         no Metal GPU backend       (--with-metal)
      --no-gpu           same as --no-cuda --no-metal
      --minimal          all of the above off: file viewer, simulator and generator only

checks:
      --werror           compiler warnings are errors
      --sanitize KIND    address (ASan + UBSan) or thread (TSan); the folder gets a .KIND suffix
      --bb-sim           BB60 backend against the simulated device (no SDK needed)
      --fuzz             build the libFuzzer targets (clang only)

examples:
  scripts/build.sh                                  # this computer, all found features
  scripts/build.sh --arch x86_64 --minimal          # macOS: an Intel build on Apple silicon
  scripts/build.sh --no-cuda --no-uhd --test        # then run the self-checks
  scripts/build.sh --sanitize thread --type RelWithDebInfo
  scripts/build.sh -- -DCMAKE_CUDA_ARCHITECTURES=89 # extra CMake arguments
EOF
}

die() {
    echo "build.sh: $*" >&2
    exit 2
}

normArch() {
    case "$1" in
        x86_64 | amd64 | x64) echo x86_64 ;;
        arm64 | aarch64) echo arm64 ;;
        *) return 1 ;;
    esac
}

host=$(normArch "$(uname -m)") || die "unknown host architecture $(uname -m)"
os=$(uname -s)
arch=$host
type=Release
jobs=""
dir=""
toolchain=""
clean=0
test=0
dry=0
sanitize=""
cmakeArgs=()
extra=()

feature() {  # feature NAME VALUE: WVT_WITH_<NAME>=<VALUE>
    cmakeArgs+=("-DWVT_WITH_$1=$2")
}

while [ $# -gt 0 ]; do
    case "$1" in
        -a | --arch) [ $# -ge 2 ] || die "$1 needs a value"; arch=$(normArch "$2") || die "unknown architecture '$2' (x86_64 or arm64)"; shift ;;
        -t | --type)
            [ $# -ge 2 ] || die "$1 needs a value"
            case "$2" in Release | Debug | RelWithDebInfo | MinSizeRel) type=$2 ;; *) die "unknown build type '$2'" ;; esac
            shift ;;
        -j | --jobs)
            [ $# -ge 2 ] || die "$1 needs a value"
            case "$2" in '' | *[!0-9]*) die "$1 wants a whole number" ;; esac
            jobs=$2; shift ;;
        -B | --build-dir) [ $# -ge 2 ] || die "$1 needs a value"; dir=$2; shift ;;
        --toolchain) [ $# -ge 2 ] || die "$1 needs a value"; toolchain=$2; shift ;;
        --clean) clean=1 ;;
        --test) test=1 ;;
        -n | --dry-run) dry=1 ;;
        -h | --help) usage; exit 0 ;;
        --no-rtlsdr) feature RTLSDR OFF ;;
        --no-uhd) feature UHD OFF ;;
        --no-bb) feature BB OFF ;;
        --no-audio) feature AUDIO OFF ;;
        --no-cuda) feature CUDA OFF ;;
        --no-metal) feature METAL OFF ;;
        --no-gpu) feature CUDA OFF; feature METAL OFF ;;
        --minimal) for f in RTLSDR UHD BB AUDIO CUDA METAL; do feature $f OFF; done ;;
        --with-rtlsdr) feature RTLSDR ON ;;
        --with-uhd) feature UHD ON ;;
        --with-bb) feature BB ON ;;
        --with-audio) feature AUDIO ON ;;
        --with-cuda) feature CUDA ON ;;
        --with-metal) feature METAL ON ;;
        --werror) cmakeArgs+=(-DWVT_WERROR=ON) ;;
        --bb-sim) cmakeArgs+=(-DWVT_BB_SIM=ON) ;;
        --fuzz) cmakeArgs+=(-DWVT_FUZZ=ON) ;;
        --sanitize)
            [ $# -ge 2 ] || die "$1 needs a value"
            case "$2" in address | thread) sanitize=$2 ;; *) die "--sanitize wants address or thread" ;; esac
            shift ;;
        --) shift; extra=("$@"); break ;;
        *) die "unknown option '$1' (try --help)" ;;
    esac
    shift
done

if [ -n "$sanitize" ]; then cmakeArgs+=("-DWVT_SANITIZE=$sanitize"); fi
if [ -n "$toolchain" ]; then cmakeArgs+=("-DCMAKE_TOOLCHAIN_FILE=$toolchain"); fi

# Another architecture: macOS compiles for it directly; Linux needs a cross
# toolchain (compiler, sysroot and libraries for that architecture).
if [ "$arch" != "$host" ]; then
    if [ "$os" = Darwin ]; then
        echo "build.sh: note: Homebrew libraries are for $host only. If a library is found for $host," \
             "the link fails: turn it off (for example --minimal) or install $arch versions." >&2
    elif [ -z "$toolchain" ]; then
        die "a $arch build on a $host Linux needs --toolchain FILE (a CMake cross toolchain)"
    fi
fi
if [ "$os" = Darwin ]; then cmakeArgs+=("-DCMAKE_OSX_ARCHITECTURES=$arch"); fi

if [ -z "$dir" ]; then
    dir="$root/build.$arch"
    if [ -n "$sanitize" ]; then dir="$dir.$sanitize"; fi
fi
if [ -z "$jobs" ]; then
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
fi

run() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    if [ "$dry" = 0 ]; then "$@"; fi
}

if [ "$clean" = 1 ] && [ -d "$dir" ]; then run rm -rf "$dir"; fi
run cmake -S "$root" -B "$dir" "-DCMAKE_BUILD_TYPE=$type" ${cmakeArgs[@]+"${cmakeArgs[@]}"} ${extra[@]+"${extra[@]}"}
run cmake --build "$dir" -j "$jobs"
if [ "$test" = 1 ]; then run ctest --test-dir "$dir" --output-on-failure; fi
if [ "$dry" = 0 ]; then echo "build.sh: wvt is in $dir"; fi
