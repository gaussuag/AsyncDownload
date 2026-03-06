#!/bin/bash

set -e

BUILD_DIR="build"

clean() {
    if [ -d "$BUILD_DIR" ]; then
        rm -rf "$BUILD_DIR"
        echo "Build directory cleaned."
    fi
}

configure() {
    if [ ! -d "$BUILD_DIR" ]; then
        mkdir -p "$BUILD_DIR"
    fi
    cmake -S . -B "$BUILD_DIR"
}

build() {
    cmake --build "$BUILD_DIR" --config Debug
}

test() {
    ctest --test-dir "$BUILD_DIR" --output-on-failure
}

case "${1:-build}" in
    clean)
        clean
        ;;
    configure)
        configure
        ;;
    test)
        test
        ;;
    build)
        configure
        build
        ;;
    all)
        configure
        build
        test
        ;;
    *)
        echo "Usage: $0 {clean|configure|build|test|all}"
        exit 1
        ;;
esac
