#!/bin/sh
# sources the android build compiles itself (android/app/src/main/cpp/CMakeLists.txt):
# SDL (also its java activity), lua 5.3, libmonome. the other libraries come from vcpkg
# (android/app/src/main/cpp/vcpkg.json). run from anywhere; clones into android/deps.
set -e
cd "$(dirname "$0")"
mkdir -p deps
fetch() { # dir url tag
    if [ ! -d "deps/$1/.git" ]; then
        git clone -q --depth 1 --branch "$3" "$2" "deps/$1"
    fi
}
fetch SDL https://github.com/libsdl-org/SDL.git release-2.30.9
fetch lua https://github.com/lua/lua.git v5.3.6
fetch libmonome https://github.com/monome/libmonome.git main
# norns' own submodule: lua-cjson
cd ..
git submodule update --init --depth 1 third-party/lua-cjson
# scripts the app comes with (added to dust/code when missing): awake, mlr
cd android
mkdir -p deps/dust/code
seed() { # name url
    if [ ! -d "deps/dust/code/$1" ]; then
        git clone -q --depth 1 "$2" "deps/dust/code/$1"
        rm -rf "deps/dust/code/$1/.git"
    fi
}
seed awake https://github.com/tehn/awake.git
seed mlr https://github.com/tehn/mlr.git
