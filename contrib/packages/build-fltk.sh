#!/bin/sh
set -eu

cmake -S "$1" -B "$2" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFLTK_BACKEND_WAYLAND=OFF \
    -DFLTK_BUILD_SHARED_LIBS=OFF \
    -DFLTK_BUILD_FORMS=OFF \
    -DFLTK_BUILD_FLUID=OFF \
    -DFLTK_BUILD_FLTK_OPTIONS=OFF \
    -DFLTK_BUILD_TEST=OFF \
    -DFLTK_BUILD_GL=OFF
cmake --build "$2" --target fltk_images
