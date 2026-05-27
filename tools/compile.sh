#!/usr/bin/env bash

set -e

###############################################################################

function compile-module {
  echo -e "Compiling module for \033[7m${PLATFORM}\033[0m..."
  cp -R /input /tmp
  make -C ${KSRC} M=/tmp/input ${PLATFORM^^}-Y=y ${PLATFORM^^}-M=m modules
  while read F; do
    strip -g "${F}"
    echo "Copying `basename ${F}`"
    cp "${F}" "/output"
    chown 1000:1000 "/output/`basename ${F}`"
  done < <(find /tmp/input -name \*.ko)
}

###############################################################################

function compile-lkm {
  local platform="$1"
  local target="$2"
  local kernel_src="${LINUX_SRC}"
  local nested_build=""

  if [ -z "${platform}" ] || [ -z "${target}" ]; then
    echo "Use: compile-lkm <platform> <dev|prod>"
    return 1
  fi

  if [ "${target}" != "dev" ] && [ "${target}" != "prod" ]; then
    echo "Invalid target: ${target} (expected dev or prod)"
    return 1
  fi

  # Some images export platform-scoped kernel paths (/opt/<platform>/build).
  # Prefer them when available, otherwise keep the generic DSM toolkit path.
  if [ -d "/opt/${platform}/build" ]; then
    kernel_src="/opt/${platform}/build"
  else
    # Newer images may keep build under an extra DSM-* directory.
    nested_build=$(find "/opt/${platform}" -maxdepth 6 -type d -name build 2>/dev/null | head -n 1)
    if [ -n "${nested_build}" ] && [ -d "${nested_build}" ]; then
      kernel_src="${nested_build}"
    fi
  fi

  if [ ! -d "${kernel_src}" ]; then
    echo "Kernel source path not found: ${kernel_src}"
    echo "Known toolkit paths:"
    ls -d /usr/local/x86_64-pc-linux-gnu/x86_64-pc-linux-gnu/sys-root/usr/lib/modules/*/build 2>/dev/null || true
    ls -d /opt/*/build 2>/dev/null || true
    ls -d /opt/*/*/build 2>/dev/null || true
    ls -d /opt/*/*/*/build 2>/dev/null || true
    return 1
  fi

  cp -R /input /tmp
  LINUX_SRC="${kernel_src}" KSRC="${kernel_src}" make -C "/tmp/input" clean
  PLATFORM="${platform}" LINUX_SRC="${kernel_src}" KSRC="${kernel_src}" make -C "/tmp/input" "${target}-v7"
  strip -g "/tmp/input/redpill.ko"
  mv "/tmp/input/redpill.ko" "/output/redpill.ko"
  chown 1000:1000 /output/redpill.ko
}

###############################################################################

if [ $# -lt 1 ]; then
  echo "Use: <command> (<params>)"
  echo "Commands: shell | compile-module | compile-lkm"
  exit 1
fi

if [ -z "${TOOLKIT_VER}" ]; then
  DETECTED_DSM_BUILD=$(ls -d /usr/local/x86_64-pc-linux-gnu/x86_64-pc-linux-gnu/sys-root/usr/lib/modules/DSM-*/build 2>/dev/null | head -n 1)
  if [ -n "${DETECTED_DSM_BUILD}" ]; then
    TOOLKIT_VER=$(basename "$(dirname "${DETECTED_DSM_BUILD}")" | sed 's/^DSM-//')
  fi
fi

if [ -z "${TOOLKIT_VER}" ]; then
  echo "TOOLKIT_VER is not set and could not be auto-detected"
  exit 1
fi

export PATH="${PATH}:/usr/local/x86_64-pc-linux-gnu/bin"
export KSRC="/usr/local/x86_64-pc-linux-gnu/x86_64-pc-linux-gnu/sys-root/usr/lib/modules/DSM-${TOOLKIT_VER}/build"
export LINUX_SRC="/usr/local/x86_64-pc-linux-gnu/x86_64-pc-linux-gnu/sys-root/usr/lib/modules/DSM-${TOOLKIT_VER}/build"
export CROSS_COMPILE="/usr/local/x86_64-pc-linux-gnu/bin/x86_64-pc-linux-gnu-"
#export CFLAGS="-I/opt/${1}/include"
#export LDFLAGS="-I/opt/${1}/lib"
#export LD_LIBRARY_PATH="/opt/${1}/lib"
export ARCH=x86_64
export CC="x86_64-pc-linux-gnu-gcc"
export LD="x86_64-pc-linux-gnu-ld"

case $1 in
  shell) shift && bash -l $@ ;;
  compile-module) compile-module ;;
  compile-lkm) shift && compile-lkm "$@" ;;
  *) echo "Command not recognized: $1" ;;
esac
