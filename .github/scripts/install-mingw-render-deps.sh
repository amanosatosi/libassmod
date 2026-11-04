#!/usr/bin/env bash
set -euo pipefail

pre="${MINGW_PACKAGE_PREFIX:?Run inside the selected MSYS2 environment}"
for spec in fribidi:1.0.16-1 fontconfig:2.17.1-1; do
    package="${pre}-${spec%%:*}"
    if pacman -Si "$package" >/dev/null 2>&1; then
        pacman --noconfirm --needed -S "$package"
    elif [[ "$MSYSTEM" == MINGW32 && "$pre" == mingw-w64-i686 ]]; then
        # These i686 packages were removed from the rolling package database.
        # Keep testing real 32-bit MinGW with signed official archive packages;
        # do not replace the job with a 64-bit build or disable font providers.
        archive="${package}-${spec#*:}-any.pkg.tar.zst"
        directory="$(mktemp -d)"
        url="https://repo.msys2.org/mingw/mingw32/$archive"
        curl --fail --location --retry 3 --output "$directory/$archive" "$url"
        curl --fail --location --retry 3 --output "$directory/$archive.sig" "$url.sig"
        # pacman-key enforces a Unix root UID even on Windows CI. gpgv uses
        # the same pacman-managed trusted keyring without modifying it.
        gpgv --keyring /etc/pacman.d/gnupg/pubring.gpg \
            "$directory/$archive.sig" "$directory/$archive"
        pacman --noconfirm --needed -U "$directory/$archive"
    else
        echo "Required package missing from $MSYSTEM: $package" >&2
        exit 1
    fi
done

pkg-config --print-errors --atleast-version=0.19.0 fribidi
pkg-config --print-errors --atleast-version=2.10.92 fontconfig
