# `nullspace`

This is a fluid compositor for Unix. It features bespoke visuals, robust window management with tiling/floating mode, an intuitive control system, DE integrations ([TopBar](https://codeberg.org/charlesrocket/topbar)), custom rules, and much more.

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

### Environment variables

* `NULLSPACE_VERBOSE` | _`false`_/`true` - debug output toggle
* `NULLSPACE_LOG_LEVEL` | `debug`/_`info`_/`warn`/`error`/`trace` - logging levels
* `NULLSPACE_HEADLESS_SIZE` | _`1280x720`_ - headless mode dimensions

### Controller interface
```
nctl list
```

## Contributing

Patches are accepted via [Codeberg](https://codeberg.org/charlesrocket/nullspace) or e-mail.

* [Zulip](https://nullspace.zulipchat.com/)
