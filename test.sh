#!/bin/sh
set -e

if [ $# -ne 2 ]; then
  echo "Usage: $0 os cpu" >&2
  echo "  os:  mac, linux" >&2
  echo "  cpu: x64, arm64" >&2
  exit 1
fi

os=$1
cpu=$2

case "$os" in
  mac|linux) ;;
  *)
    echo "Invalid os: $os (use mac or linux)" >&2
    exit 1
    ;;
esac

case "$cpu" in
  x64|arm64) ;;
  *)
    echo "Invalid cpu: $cpu (use x64 or arm64)" >&2
    exit 1
    ;;
esac

cd "$(dirname "$0")" || exit 1
out_dir="out.gn/${os}.${cpu}.release"
lib_path="build/v8/$out_dir/obj/libv8_monolith.a"
if [ ! -f "$lib_path" ]; then
  echo "Library not found: $lib_path (run build.sh first)" >&2
  exit 1
fi
echo "Building test binary for $os/$cpu..."
mkdir -p build/v8/test_build
(
  cd build/v8/test_build
  extra_libs=""
  case "$os" in
    mac) extra_libs="-framework CoreFoundation" ;;
    linux) extra_libs="" ;;
  esac
  c++ -std=c++23 \
    -I../include -I../include/libplatform \
    ../../../test.cc \
    "../$out_dir/obj/libv8_monolith.a" \
    $extra_libs \
    -o v8_test
)
echo "Running test..."
(cd build/v8/test_build && ./v8_test)
echo "Test passed. To clean run rm -rf build/"
