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

# Read V8 build args to detect configuration
args_file="build/v8/$out_dir/args.gn"
v8_defines=""
if grep -q 'v8_enable_pointer_compression.*=.*true' "$args_file" 2>/dev/null; then
  v8_defines="$v8_defines -DV8_COMPRESS_POINTERS"
fi
if grep -q 'v8_enable_sandbox.*=.*true' "$args_file" 2>/dev/null; then
  v8_defines="$v8_defines -DV8_ENABLE_SANDBOX"
fi

echo "Building test binary for $os/$cpu..."
mkdir -p build/v8/test_build

# Platform-specific link flags
extra_libs=""
case "$os" in
  mac) extra_libs="-framework CoreFoundation -framework Security" ;;
  linux) extra_libs="-lpthread -ldl" ;;
esac

# Detect custom libc++ (V8 built with use_custom_libcxx=true)
custom_libcxx="build/v8/$out_dir/obj/buildtools/third_party/libc++/libc++.a"
custom_libcxxabi="build/v8/$out_dir/obj/buildtools/third_party/libc++abi/libc++abi.a"

if [ -f "$custom_libcxx" ]; then
  # Two-stage build: wrapper uses V8's libc++, test.cc uses system libc++.
  V8CXX="build/v8/third_party/llvm-build/Release+Asserts/bin/clang++"
  sdk_path="$(xcrun --sdk macosx --show-sdk-path)"

  v8_cxx_flags="-nostdinc++ -nostdlib++ --target=${cpu}-apple-macos -isysroot $sdk_path"
  v8_cxx_flags="$v8_cxx_flags -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE"
  v8_cxx_flags="$v8_cxx_flags -I build/v8/buildtools/third_party/libc++ -isystem build/v8/third_party/libc++/src/include -isystem build/v8/third_party/libc++abi/src/include"

  echo "  Compiling v8_wrapper.cc (V8's clang + V8's libc++)..."
  $V8CXX -std=c++20 -c $v8_cxx_flags $v8_defines \
    -I build/v8/include -I build/v8/include/libplatform \
    v8_wrapper.cc \
    -o build/v8/test_build/v8_wrapper.o

  echo "  Compiling test.cc (system clang + system libc++)..."
  c++ -std=c++23 -c \
    -I . \
    test.cc \
    -o build/v8/test_build/test.o

  echo "  Linking with V8's lld..."
  $V8CXX --target=${cpu}-apple-macos -isysroot $sdk_path -fuse-ld=lld \
    build/v8/test_build/test.o \
    build/v8/test_build/v8_wrapper.o \
    "$lib_path" \
    "$custom_libcxx" "$custom_libcxxabi" \
    -lc++ \
    $extra_libs \
    -o build/v8/test_build/v8_test
else
  # Simple build: everything uses system libc++
  c++ -std=c++23 $v8_defines \
    -I . -I build/v8/include -I build/v8/include/libplatform \
    v8_wrapper.cc test.cc \
    "$lib_path" \
    -lc++ $extra_libs \
    -o build/v8/test_build/v8_test
fi

echo "Running test..."
(cd build/v8/test_build && ./v8_test)
echo "Test passed. To clean run rm -rf build/"
