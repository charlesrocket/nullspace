# `nullspace`

Tiling compositor for FreeBSD. It features bespoke visuals, animations, DE integrations ([TopBar](https://codeberg.org/charlesrocket/topbar)), custom rules, and much more.

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
meson setup build # -Dbuildtype=release
ninja -C build
meson install -C build
```

## Usage

```
nullspace-start
```

### IPC
```
nctl list
```

## Contributing

Patches are accepted via [Codeberg](https://codeberg.org/charlesrocket/nullspace) or e-mail.
