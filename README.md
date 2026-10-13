# `nullspace`

Tiling compositor for FreeBSD.

## Dependencies

- pkg-config
- meson
- ninja
- clang
- wayland
- wlroots
- scenefx
- xkbcommon
- libinput
- pixman
- spng

## Build

```sh
meson setup build
ninja -C build
meson install -C build
```

## Usage

```
nullspace
```

### IPC
```
nctl list
```

## Contributing

Patches are accepted via [Codeberg](https://codeberg.org/charlesrocket/nullspace) or e-mail.
