# Changelog

All notable changes to this project will be documented in this file.

## [0.1.1] - 2026-10-02

### Bug Fixes

- Set animations via ipc
- Destroy `node`
- Sanitize wallpaper path
- Update focus switch

### Refactor

- Move `main()`
- Move `Seat`
- Move `LibinputConfig`
- Move `WallpaperConfig`
- Move management structs
- Rename animation functions
- Move binding structs
- Rename main output getter

## [0.1.0] - 2026-09-30

### Bug Fixes

- Use premade variants
- Resolve implicit conversions
- Check `DBUS_SESSION_BUS_ADDRESS`
- Drop second buffer
- Improve exclusive zone blur
- Handle non-origin outputs
- Improve ppm checks
- Add surface checks
- Check `dev`
- Bump `libinput` interface
- Improve arithmetic checks
- Avoid undefined values
- Add ppm buffer
- Update wallpaper variable
- Improve caching
- Drop trimming layout
- Rewrite animations
- Improve space animations
- Animate spaces in floating mode
- Improve floating mode tracking

### Documentation

- Update description
- Add `Contributing`
- Update river command

### Features

- Add basic wallpaper infra
- Add tiling layout
- Implement layer shell
- Implement focus stack
- Add start script
- Add trimming layout
- Add animations
- Add layouts
- Add wallpaper pattern
- Blur exclusive zone
- Add spaces
- Add wallpaper script
- Animate spaces
- Add input infra
- Add global config
- Implement `libinput`
- Implement ipc
- Add fx/input variables

### Miscellaneous tasks

- Add license

### Operations

- Add pull request template
- Add release job
- Fix `release` job

### Refactor

- Move source files
- `wallpaper` -> `wp`
- Move animations
- Move `Output`

### Styling

- Align bitfields
- Fix layout formatting

### Build

- Update flags
- Add `wallpaper` option
- Move executable
- Fix wallpaper option
- Fix version string


