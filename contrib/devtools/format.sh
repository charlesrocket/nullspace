#!/usr/bin/env sh

find contrib -type f \( -iname '*.h' -o -iname '*.c' \) -exec clang-format -i {} +
find include -type f \( -iname '*.h' -o -iname '*.c' \) -exec clang-format -i {} +
find src -type f \( -iname '*.h' -o -iname '*.c' \) -exec clang-format -i {} +
