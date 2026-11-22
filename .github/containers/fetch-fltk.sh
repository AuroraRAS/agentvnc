#!/bin/sh
set -eu

archive=$1
curl -fsSL https://github.com/fltk/fltk/archive/refs/tags/release-1.4.5.tar.gz -o "$archive"
printf '%s  %s\n' 7715e69ce081fa9ce6da48bb0dd3b07a4cf2cf937813814c04272f36fff593ea "$archive" | sha256sum -c -
