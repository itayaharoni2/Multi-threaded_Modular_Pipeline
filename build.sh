#!/usr/bin/env bash
# Ensure that if any gcc command fails, the script exits with a non-zero exit code 
set -euo pipefail

# ---------- Helpers ----------
print_status() { echo -e "[BUILD] $*"; }
print_error()  { echo -e "[ERROR] $*" >&2; }

# ---------- Config ----------
MAIN_SRC="main.c"
OUT_DIR="output"

# Feature included so strdup/usleep are declared on glibc
CFLAGS=${CFLAGS:- -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L}

# Plugin names
PLUGINS=(logger uppercaser rotator flipper expander typewriter)

# Map plugin name to source file
declare -A SRC=(
  [logger]="plugins/logger.c"
  [uppercaser]="plugins/uppercase.c"
  [rotator]="plugins/rotator.c"
  [flipper]="plugins/flipper.c"
  [expander]="plugins/expander.c"
  [typewriter]="plugins/typewriter.c"
)


# ---------- Compiler: require GCC 13 ----------
require_major=13

# Prefer gcc-13 if present, otherwise dont collapse
if command -v gcc-13 >/dev/null 2>&1; then
  CC=${CC:-gcc-13}
else
  CC=${CC:-gcc}
fi

# Reject clang masquerading as gcc
if "$CC" -v 2>&1 | grep -qi clang; then
  print_error "Clang detected at '$CC'. GCC ${require_major} is required."
  exit 1
fi

# Extract major version robustly
full_ver=$("$CC" -dumpfullversion 2>/dev/null || "$CC" -dumpversion)
major_ver=${full_ver%%.*}

if [[ "$major_ver" != "$require_major" ]]; then
  print_error "Found $CC version ${full_ver}. GCC ${require_major} is required."
  echo "        Hint: install gcc-${require_major} and run: CC=gcc-${require_major} ./build.sh" >&2
  exit 1
fi

print_status "Using CC: ${CC} (version ${full_ver})"


# ---------- Test to make sure ----------
if [[ ! -f "${MAIN_SRC}" ]] || [[ ! -d "plugins" ]]; then
  print_error "Run from project root (expect '${MAIN_SRC}' and 'plugins/')."
  exit 1
fi


# makes sure every build runs over the previous output dir (delete prev)
rm -rf "${OUT_DIR}"
# create the output directory
mkdir -p "${OUT_DIR}"


# ---------- Build main into the output directory ----------
print_status "Building analyzer → ${OUT_DIR}/analyzer"
${CC} ${CFLAGS} -o "${OUT_DIR}/analyzer" "${MAIN_SRC}" -ldl


# ---------- Build plugins like the instructions ----------
for name in "${PLUGINS[@]}"; do
  src="${SRC[$name]:-}"
  if [[ -z "${src}" ]]; then
    print_error "No source mapped for plugin '${name}'."
    exit 1
  fi
  if [[ ! -f "${src}" ]]; then
    print_error "Missing ${src}"
    exit 1
  fi

  print_status "Building plugin: ${name} → ${OUT_DIR}/${name}.so"
  ${CC} -fPIC -shared ${CFLAGS} -o "${OUT_DIR}/${name}.so" \
    "${src}" \
    "plugins/plugin_common.c" \
    "plugins/sync/monitor.c" \
    "plugins/sync/consumer_producer.c" \
    -ldl -lpthread
done

echo
print_status "Build succeeded."