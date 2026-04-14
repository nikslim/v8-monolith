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
    mac) extra_libs="-framework CoreFoundation -framework Security" ;;
    linux) extra_libs="-lpthread -ldl" ;;
  esac

  # Read V8 build args to detect configuration
  args_file="../$out_dir/args.gn"
  v8_defines=""
  if grep -q 'v8_enable_pointer_compression.*=.*true' "$args_file" 2>/dev/null; then
    v8_defines="$v8_defines -DV8_COMPRESS_POINTERS"
  fi
  if grep -q 'v8_enable_sandbox.*=.*true' "$args_file" 2>/dev/null; then
    v8_defines="$v8_defines -DV8_ENABLE_SANDBOX"
  fi

  # If V8 was built with custom libc++ (use_custom_libcxx=true), compile and link
  # against V8's libc++ so both sides share the same std:: (std::__Cr namespace).
  # Use V8's bundled clang+lld which support thin archives natively.
  custom_libcxx="../$out_dir/obj/buildtools/third_party/libc++/libc++.a"
  custom_libcxxabi="../$out_dir/obj/buildtools/third_party/libc++abi/libc++abi.a"
  CXX="c++"
  cxx_flags=""
  if [ -f "$custom_libcxx" ]; then
    CXX="../third_party/llvm-build/Release+Asserts/bin/clang++"
    sdk_path="$(xcrun --sdk macosx --show-sdk-path)"
    cxx_flags="-nostdinc++ -nostdlib++ --target=${cpu}-apple-macos -isysroot $sdk_path -fuse-ld=lld"
    cxx_flags="$cxx_flags -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE"
    cxx_flags="$cxx_flags -I../buildtools/third_party/libc++ -isystem../third_party/libc++/src/include -isystem../third_party/libc++abi/src/include"
    extra_libs="$custom_libcxx $custom_libcxxabi $extra_libs"
  else
    extra_libs="-lc++ $extra_libs"
  fi

  $CXX -std=c++23 $cxx_flags $v8_defines \
    -I../include -I../include/libplatform \
    ../../../test.cc \
    "../$out_dir/obj/libv8_monolith.a" \
    $extra_libs \
    -o v8_test
)
echo "Running test..."
(cd build/v8/test_build && ./v8_test)
echo "Test passed. To clean run rm -rf build/"
