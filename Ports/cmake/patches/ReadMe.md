# Patches for CMake (and submodules) on SerenityOS

## `0000-no_wide_string.patch`

We don't support wide strings, and our libstdc++ doesn't have `std::wstring`.
This patch is a big hack to wipe wide strings out of the codebase; naturally, it very likely breaks unicode.

### Status
- [ ] Local?
- [ ] Should be merged to upstream?
- [X] Resolves issue(s) with our side of things
- [X] Hack

## `0008-unix-stuff.patch`

This patch removes the use of `{get,set}priority()` as we do not support it.
it also removes two functions with the correct conditions (the same conditions as their dependents are removed with).

### Status
- [X] Local?
- [X] Should be merged to upstream? Partially.
- [X] Resolves issue(s) with our side of things
- [X] Hack

## `0009-purge-non-serenity-syscalls.patch`

This patch removes syscalls and options not defined in serenity.

### Status
- [X] Local?
- [ ] Should be merged to upstream?
- [X] Resolves issue(s) with our side of things
- [X] Hack

## `0010-don-t-use-siginfo.patch`

We don't support SIGINFO. This patch removes uses of SIGINFO.

### Status
- [X] Local?
- [ ] Should be merged to upstream?
- [X] Resolves issue(s) with our side of things
- [ ] Hack

## `0011-Fixed-your-code-rot-cmake.patch`

This purely fixes code issues with cmake. very funny patch.

### Status
- [ ] Local?
- [X] Should be merged to upstream?
- [ ] Resolves issue(s) with our side of things
- [ ] Hack

## `0012-bin-bash.patch`

This patch swaps out `/bin/sh` for `/bin/bash` in two scripts that need it.

### Status
- [X] Local?
- [ ] Should be merged to upstream?
- [ ] Resolves issue(s) with our side of things
- [ ] Hack

## `0013-platform-serenityos.patch`
This patch adds the SerenityOS platform config file to CMake.

### Status
- [ ] Local?
- [X] Should be merged to upstream? If we want to have cmake support serenity out of the box.
- [ ] Resolves issue(s) with our side of things
- [ ] Hack

## `0014-cmcurl-include-unistd.patch`

Everyone gets this wrong. most platforms are very lax with these includes, but we're not one of them.

### Status
- [X] Local?
- [ ] Should be merged to upstream?
- [ ] Resolves issue(s) with our side of things
- [ ] Hack

## `0016-conflicting-0.patch` and `0017-conflicting-1.patch`

These two defines make GCC very sad. reasons are unknown at this time.

### Status
- [ ] Local?
- [ ] Should be merged to upstream?
- [X] Resolves issue(s) with our side of things
- [X] Hack

## `0019-libuv-so_linger.patch`

We don't have `SO_LINGER` or its associated struct. This patch removes them.

### Status
- [X] Local?
- [ ] Should be merged to upstream?
- [X] Resolves issue(s) with our side of things
- [X] Hack

## `0025-uv-platform.patch`

This patch adds the definitions necessary to compile libuv on Serenity.

### Status
- [ ] Local?
- [X] Should be merged to upstream? If we want to have cmake support serenity out of the box.
- [ ] Resolves issue(s) with our side of things
- [ ] Hack

## `0026-curl-struct-stat.patch`

For unknown reasons, `curl_setup_once.h` does not include `sys/stat.h`. this patch includes `sys/stat.h`.

### Status
- [ ] Local?
- [ ] Should be merged to upstream?
- [X] Resolves issue(s) with our side of things
- [X] Hack

## `0027-libuv-platform-serenity.patch`

This patch adds a platform-specific conditional include to `uv/unix.h`.

### Status
- [ ] Local?
- [X] Should be merged to upstream?
- [ ] Resolves issue(s) with our side of things
- [ ] Hack

## `0028-cmake-disable-tests.patch`

We don't care about building tests for now, and it makes the compilation much faster.

### Status
- [ ] Local?
- [ ] Should be merged to upstream?
- [ ] Resolves issue(s) with our side of things
- [ ] Hack
