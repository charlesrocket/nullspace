# `nullspace`

Tiny river window manager implemented in C.

## Dependencies

The following system dependencies are required:

- pkg-config
- meson
- ninja
- wayland
- xkbcommon

## Building

```sh
meson setup build
ninja -C build
```

## Running

```
river -c ./build/nullspace
```
