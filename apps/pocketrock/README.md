# PocketRock firmware adapter

This directory is the GPL Rockbox side of PocketRock. It owns the 16 MiB
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
