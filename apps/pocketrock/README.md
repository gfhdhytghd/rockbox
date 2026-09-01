# PocketRock firmware adapter

This directory is the GPL Rockbox side of PocketRock. It owns the 8 MiB
`core_alloc` arena, package validation, adaptive render loop, Rockbox services,
native plugin handoff, logging, and recovery UI. QuickJS, PocketJS runtime
sources, the Rust core archive, and embedded Shell data are generated from the
PocketJS repository into the ignored `generated/` directory before a firmware
build.

The adapter is restricted to `IPOD_6G`. Without
`HAVE_POCKETROCK_RUNTIME`, weak guest functions enter native recovery instead
of linking an incomplete JavaScript host.

Build orchestration and the pinned dependency revisions are documented in the
PocketJS repository at `docs/POCKETROCK.md`.

`POCKETROCK_MINIMAL_UI` suppresses native boot progress, voice notices, and
skin loading. The standard Rockbox root menu is never entered. A small direct
framebuffer recovery screen remains available when Menu is held at boot or the
guest repeatedly fails.

Rockbox list, menu, keyboard, browser, and skin implementations remain linked
as a compatibility layer because the stable `.rock` ABI exposes them through
`struct plugin_api`. Removing those functions would preserve the struct layout
but make existing plugins crash when they call the missing entries. Before a
native plugin starts, PocketRock releases the JavaScript arena and applies its
fixed compatibility theme; user-selected Rockbox themes are never loaded.
