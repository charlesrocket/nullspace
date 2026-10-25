# Changelog

All notable changes to this project will be documented in this file.

## [0.2.0] - 2026-10-25

### Bug Fixes

- Add libinput result listener
- Use logging macro
- Update animation settings
- Update destroyers
- Init kevent struct
- Change startup space
- Improve list formatting
- Update instance signature
- Fix focus on close
- Sync animation speed with topbar
- Improve animations on close
- Activate xdg toplevel
- Handle popups
- Handle foreign toplevel
- Wire up idle notifier
- Set toplevel base
- Track node type
- Move `node.data`
- Improve buffering/tables
- Improve socket writes/buffering
- Update env vars
- Import session header
- Drop space counts
- Fine-tune blur settings
- Adjust blur radius
- Add popup tree
- Fix inconsistent decorations
- Destroy window surface data
- Improve focus guards
- Switch to pre-scenefx `box_blur()`
- Properly handle `EBUSY`
- Correct exclusive zone placements
- Fix default `ACTION_EXIT`

### Documentation

- Add build type

### Features

- Add default bindings
- Expose socket
- Add logger
- Add `nctl`
- Switch to scenefx
- Add png support
- Add background blur toggle
- Implement power management
- Add power commands
- Implement fullscreen action
- Implement session lock
- Add rounded corners
- Add focus follow
- Add popup shadows
- Add window rules
- Add xwayland support
- Add start command
- Implement xdg activation
- Implement ext-workspace
- Add `CFG_GAPS`
- Add config file
- Implement primary selection interface
- Implement fractional scaling

### Operations

- Add formatter

### Performance

- Gate blur passes
- Improve dirty checks

### Refactor

- Fix imports
- Fix unused params
- Update layout helpers
- Drop redundant imports
- Simplify `animation_ease()`
- Drop header guards
- Move `Window` objects
- Move `window_from_scene_node()`
- Move `output_from_wlr_output()`
- Extract `Popup`
- Define FN keys

### Styling

- Fix macros formatting
- Fix imports formatting

### Build

- Add version string
- Add `asan` option
- Gate memory sanitizer
- Gate `spng`
- Enable `FORTIFY_SOURCE`
- Move wlr flag

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


