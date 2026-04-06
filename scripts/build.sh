#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${WORK_DIR:-$ROOT_DIR/work}"
UPSTREAM_DIR="${UPSTREAM_DIR:-$WORK_DIR/rockbox}"
PATCH_DIR="${PATCH_DIR:-$ROOT_DIR/patches}"
OVERLAY_DIR="${OVERLAY_DIR:-$ROOT_DIR/overlay}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out}"
CCACHE_DIR="${CCACHE_DIR:-$ROOT_DIR/.ccache}"

ROCKBOX_REMOTE="${ROCKBOX_REMOTE:-https://github.com/Rockbox/rockbox.git}"
ROCKBOX_BASE_COMMIT="${ROCKBOX_BASE_COMMIT:-9a6e3799e18ba1e365cb23f6a2a5044e49cecffc}"

TOOLCHAIN_PREFIX="${TOOLCHAIN_PREFIX:-$ROOT_DIR/toolchain}"
RBDEV_DOWNLOAD="${RBDEV_DOWNLOAD:-$WORK_DIR/rbdev-download}"
RBDEV_BUILD="${RBDEV_BUILD:-$WORK_DIR/rbdev-build}"
GNU_MIRROR="${GNU_MIRROR:-https://ftp.gnu.org/gnu}"
LINUX_MIRROR="${LINUX_MIRROR:-https://cdn.kernel.org/pub/linux}"
R1_UPDATE_URL="${R1_UPDATE_URL:-}"
R1_UPDATE_SHA256="${R1_UPDATE_SHA256:-}"
R1_UPDATE_PATH="${R1_UPDATE_PATH:-}"

log() {
  printf '[build] %s\n' "$*"
}

download_file() {
  local url="$1"
  local dest="$2"

  if command -v curl >/dev/null 2>&1; then
    curl -L --fail --retry 3 --retry-delay 2 -o "$dest" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "$dest" "$url"
  else
    python3 - "$url" "$dest" <<'PY'
import sys
import urllib.request

url, dest = sys.argv[1], sys.argv[2]
with urllib.request.urlopen(url) as src, open(dest, 'wb') as dst:
    while True:
        chunk = src.read(1024 * 1024)
        if not chunk:
            break
        dst.write(chunk)
PY
  fi
}

install_overlay() {
  local src rel dst

  [ -d "$OVERLAY_DIR" ] || return 0

  while IFS= read -r src; do
    rel="${src#$OVERLAY_DIR/}"
    dst="$UPSTREAM_DIR/$rel"
    mkdir -p "$(dirname "$dst")"
    cp -f "$src" "$dst"
  done < <(find "$OVERLAY_DIR" -type f | sort)
}

default_build_jobs() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
  elif command -v getconf >/dev/null 2>&1; then
    getconf _NPROCESSORS_ONLN
  else
    echo 1
  fi
}

sha256_cmd() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$@"
  else
    shasum -a 256 "$@"
  fi
}

sha256_file() {
  sha256_cmd "$1" | awk '{print $1}'
}

package_r1_upt() {
  local patcher package_dir base_upt base_sha256

  if [ -z "$R1_UPDATE_URL" ] && [ -z "$R1_UPDATE_PATH" ]; then
    log "skipping .upt packaging; no base update configured"
    return 0
  fi

  patcher="$UPSTREAM_DIR/tools/r1_patcher/r1_patcher.sh"
  package_dir="$WORK_DIR/r1-package"
  base_upt="$package_dir/r1.upt"

  if [ ! -f "$patcher" ]; then
    echo "Missing r1 patcher at $patcher" >&2
    exit 1
  fi

  if [ ! -f "$OUT_DIR/bootloader.r1" ]; then
    echo "Missing bootloader.r1 in $OUT_DIR" >&2
    exit 1
  fi

  rm -rf "$package_dir"
  mkdir -p "$package_dir"

  if [ -n "$R1_UPDATE_PATH" ]; then
    log "using local base update from $R1_UPDATE_PATH"
    cp -f "$R1_UPDATE_PATH" "$base_upt"
  else
    log "downloading base update from configured URL"
    download_file "$R1_UPDATE_URL" "$base_upt"
  fi

  if [ -n "$R1_UPDATE_SHA256" ]; then
    base_sha256="$(sha256_file "$base_upt")"
    if [ "$base_sha256" != "$R1_UPDATE_SHA256" ]; then
      echo "Base update SHA256 mismatch: expected $R1_UPDATE_SHA256 got $base_sha256" >&2
      exit 1
    fi
  fi

  cp -f "$OUT_DIR/bootloader.r1" "$package_dir/bootloader.r1"
  chmod +x "$patcher"

  log "packaging flashable .upt"
  (
    cd "$package_dir"
    bash "$patcher" r1.upt bootloader.r1
  )

  cp -f "$package_dir/r1_rb.upt" "$OUT_DIR/"
}

toolchain_smoke_test() {
  local tmpdir cfile ofile

  tmpdir="$(mktemp -d)"
  cfile="$tmpdir/toolchain-smoke.c"
  ofile="$tmpdir/toolchain-smoke.o"
  printf 'int toolchain_smoke;\n' > "$cfile"

  if "$TOOLCHAIN_PREFIX/bin/mipsel-rockbox-linux-gnu-gcc" -c "$cfile" -o "$ofile" >/dev/null 2>&1; then
    rm -rf "$tmpdir"
    return 0
  fi

  rm -rf "$tmpdir"
  return 1
}

toolchain_ready() {
  [ -x "$TOOLCHAIN_PREFIX/bin/mipsel-rockbox-linux-gnu-gcc" ] &&
    [ -f "$TOOLCHAIN_PREFIX/mipsel-rockbox-linux-gnu/sysroot/usr/include/sys/types.h" ] &&
    toolchain_smoke_test
}

setup_ccache() {
  local wrapper_dir

  if ! command -v ccache >/dev/null 2>&1; then
    return
  fi

  wrapper_dir="$WORK_DIR/ccache-wrappers"
  mkdir -p "$wrapper_dir" "$CCACHE_DIR"

  cat > "$wrapper_dir/mipsel-rockbox-linux-gnu-gcc" <<EOF
#!/usr/bin/env bash
exec ccache "$TOOLCHAIN_PREFIX/bin/mipsel-rockbox-linux-gnu-gcc" "\$@"
EOF
  chmod +x "$wrapper_dir/mipsel-rockbox-linux-gnu-gcc"

  cat > "$wrapper_dir/mipsel-rockbox-linux-gnu-g++" <<EOF
#!/usr/bin/env bash
exec ccache "$TOOLCHAIN_PREFIX/bin/mipsel-rockbox-linux-gnu-g++" "\$@"
EOF
  chmod +x "$wrapper_dir/mipsel-rockbox-linux-gnu-g++"

  export CCACHE_DIR
  export CCACHE_BASEDIR="$ROOT_DIR"
  export CCACHE_COMPILERCHECK=content
  export CCACHE_NOHASHDIR=1
  export PATH="$wrapper_dir:$TOOLCHAIN_PREFIX/bin:$PATH"

  log "ccache enabled: $CCACHE_DIR"
  ccache --zero-stats >/dev/null 2>&1 || true
}

BUILD_JOBS="${BUILD_JOBS:-$(default_build_jobs)}"

mkdir -p "$WORK_DIR" "$OUT_DIR"

if [ ! -d "$UPSTREAM_DIR/.git" ]; then
  log "initializing upstream Rockbox repo"
  git init "$UPSTREAM_DIR"
fi

# Containerized/local repros may mount the workspace with a different owner.
git config --global --add safe.directory "$UPSTREAM_DIR" >/dev/null 2>&1 || true

git -C "$UPSTREAM_DIR" remote remove origin >/dev/null 2>&1 || true
git -C "$UPSTREAM_DIR" remote add origin "$ROCKBOX_REMOTE"

log "fetching upstream base commit $ROCKBOX_BASE_COMMIT"
git -C "$UPSTREAM_DIR" fetch --force --depth 1 origin "$ROCKBOX_BASE_COMMIT"
git -C "$UPSTREAM_DIR" checkout --force FETCH_HEAD
git -C "$UPSTREAM_DIR" clean -fdx
git -C "$UPSTREAM_DIR" config user.name "rockbox-hiby-r1-ci"
git -C "$UPSTREAM_DIR" config user.email "ci@example.invalid"
(git -C "$UPSTREAM_DIR" am --abort >/dev/null 2>&1 || true)

log "installing overlay files"
install_overlay

PATCHES=()
while IFS= read -r patch; do
  PATCHES+=("$patch")
done < <(find "$PATCH_DIR" -maxdepth 1 -type f -name '*.patch' | sort)

if [ ${#PATCHES[@]} -eq 0 ]; then
  echo "No patches found in $PATCH_DIR" >&2
  exit 1
fi

log "applying ${#PATCHES[@]} patches"
for p in "${PATCHES[@]}"; do
  log "git am $(basename "$p")"
  git -C "$UPSTREAM_DIR" am "$p"
done

PATCHED_HEAD="$(git -C "$UPSTREAM_DIR" rev-parse --verify HEAD)"

if ! toolchain_ready; then
  if [ -d "$TOOLCHAIN_PREFIX" ]; then
    log "existing toolchain is unusable; rebuilding"
    rm -rf "$TOOLCHAIN_PREFIX"
  fi
  rm -rf "$RBDEV_BUILD"
  export PKG_CONFIG_SYSROOT_DIR="$TOOLCHAIN_PREFIX/mipsel-rockbox-linux-gnu/sysroot"
  export PKG_CONFIG_LIBDIR="$PKG_CONFIG_SYSROOT_DIR/usr/lib/pkgconfig:$PKG_CONFIG_SYSROOT_DIR/usr/share/pkgconfig"
  export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
  export RBDEV_PREFIX="$TOOLCHAIN_PREFIX"
  export RBDEV_DOWNLOAD
  export RBDEV_BUILD
  export GNU_MIRROR
  export LINUX_MIRROR
  log "bootstrapping toolchain via rockboxdev.sh"
  "$UPSTREAM_DIR/tools/rockboxdev.sh" --target=y
fi

setup_ccache

if ! command -v mipsel-rockbox-linux-gnu-gcc >/dev/null 2>&1; then
  export PATH="$TOOLCHAIN_PREFIX/bin:$PATH"
fi

FW_BUILD_DIR="$WORK_DIR/build-hibyr1-rockbox"
BL_BUILD_DIR="$WORK_DIR/build-hibyr1-bootloader"

rm -rf "$FW_BUILD_DIR" "$BL_BUILD_DIR"
mkdir -p "$FW_BUILD_DIR" "$BL_BUILD_DIR"

log "building firmware"
(
  cd "$FW_BUILD_DIR"
  "$UPSTREAM_DIR/tools/configure" --target=hibyr1 --type=n
  make -j"$BUILD_JOBS"
)

log "building bootloader"
(
  cd "$BL_BUILD_DIR"
  "$UPSTREAM_DIR/tools/configure" --target=hibyr1 --type=b
  make -j"$BUILD_JOBS"
)

cp -f "$FW_BUILD_DIR/rockbox.r1" "$OUT_DIR/"
cp -f "$FW_BUILD_DIR/rockbox-info.txt" "$OUT_DIR/"
[ -f "$FW_BUILD_DIR/rockbox.zip" ] && cp -f "$FW_BUILD_DIR/rockbox.zip" "$OUT_DIR/"

if [ -f "$BL_BUILD_DIR/bootloader.r1" ]; then
  cp -f "$BL_BUILD_DIR/bootloader.r1" "$OUT_DIR/"
else
  # fallback in case naming changes
  find "$BL_BUILD_DIR" -maxdepth 1 -type f -name 'bootloader*' -exec cp -f {} "$OUT_DIR/" \;
fi

package_r1_upt

cat > "$OUT_DIR/build-metadata.txt" <<META
rockbox_remote=$ROCKBOX_REMOTE
rockbox_base_commit=$ROCKBOX_BASE_COMMIT
patched_head=$PATCHED_HEAD
build_jobs=$BUILD_JOBS
gnu_mirror=$GNU_MIRROR
linux_mirror=$LINUX_MIRROR
r1_update_url=$R1_UPDATE_URL
r1_update_sha256=$R1_UPDATE_SHA256
r1_update_path=$R1_UPDATE_PATH
META

(
  cd "$OUT_DIR"
  > SHA256SUMS
  while IFS= read -r artifact; do
    sha256_cmd "$artifact"
  done < <(find . -maxdepth 1 -type f ! -name 'SHA256SUMS' -print | sed 's#^\./##' | sort) \
    > SHA256SUMS
)

log "build complete; artifacts in $OUT_DIR"
ls -l "$OUT_DIR"

if command -v ccache >/dev/null 2>&1; then
  ccache --show-stats || true
fi
