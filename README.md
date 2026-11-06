# `nullspace`

This is a tiling compositor for FreeBSD. It features bespoke visuals, robust window management, an intuitive control system, DE integrations ([TopBar](https://codeberg.org/charlesrocket/topbar)), custom rules, and much more.

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

```
meson setup build -Dbuildtype=release
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
