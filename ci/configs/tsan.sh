export LC_ALL=C.UTF-8
CI_DESC="CI job running thread sanitizer"
CI_DIR=build-tsan
export CXX=clang++
export CXXFLAGS="-ggdb -Werror -Wall -Wextra -Wpedantic -Wthread-safety-analysis -Wno-unused-parameter -fsanitize=thread"
CMAKE_ARGS=()
BUILD_ARGS=(-k -j4)
BUILD_TARGETS=(mptest)
