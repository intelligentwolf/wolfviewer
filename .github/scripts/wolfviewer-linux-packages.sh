#!/bin/bash
# [2026-10-02] Build the WolfViewer Linux packages from the bundle tarball (the linux-bundle job's
# output: standard + AVX2 viewer, wrapper.sh picks).
#
#   wolfviewer-linux-packages.sh deb|rpm|arch <bundle.tar.xz> <version, e.g. 7.2.4.53> <out dir>
#
# deb on Debian/Ubuntu (dpkg-dev), rpm wherever rpmbuild is, arch on Arch Linux as a non-root user
# with makepkg (the workflow runs it in the archlinux container).
#
# Both install the tarball's tree unchanged to /opt/wolfviewer, plus:
#   /usr/bin/wolfviewer                               runs /opt/wolfviewer/wolfviewer
#   /usr/share/applications/wolfviewer.desktop        the menu entry
#   /usr/share/applications/wolfviewer-url-handler.desktop  secondlife:// hop:// x-grid-location-info://
#   /usr/share/icons/hicolor/{48x48,512x512}/apps/wolfviewer.png
#   /opt/wolfviewer/etc/wolfviewer-package            tells the viewer (wolfupdate.cpp) that the
#                                                     system's package manager owns its updates
#
# Dependencies are not written by hand. deb: dpkg-shlibdeps over every 64-bit ELF file, with the
# tarball's own library folders as private (-l), so only what the system must supply is named.
# rpm: rpm's own ELF dependency generator, with the bundled sonames filtered out of Requires and
# nothing under /opt/wolfviewer offered as a Provides. The 32-bit native SLVoice (Vivox SDK 3.2,
# used only off Wolf Territories — WolfVoiceBackend "auto" is WebRTC here) is left out of both
# scans: naming its i386 libraries would drag a 32-bit stack onto every machine.
# arch: makepkg does not work dependencies out, so the NEEDED entries of every 64-bit ELF file
# (readelf -d) less the bundled sonames are looked up in Arch's own file database (pacman -F) and
# the owning packages become depends=().
set -euo pipefail

KIND="${1:?deb|rpm|arch}"
TARBALL="$(readlink -f "${2:?bundle tarball}")"
VERSION="${3:?version}"
OUT="$(mkdir -p "${4:?out dir}" && cd "$4" && pwd)"
[[ "$VERSION" =~ ^[0-9]+(\.[0-9]+)*$ ]] || { echo "bad version '$VERSION'" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ── the tree, common to both ─────────────────────────────────────────────────────────────
mkdir -p "$WORK/x" "$WORK/root/opt"
tar -C "$WORK/x" -xpf "$TARBALL"
shopt -s nullglob; TOPS=("$WORK"/x/*); shopt -u nullglob
[ "${#TOPS[@]}" -eq 1 ] || { echo "expected one folder in the tarball" >&2; exit 1; }
mv "${TOPS[0]}" "$WORK/root/opt/wolfviewer"
APP="$WORK/root/opt/wolfviewer"
test -x "$APP/bin/do-not-directly-run-wolfviewer-bin"
test -f "$APP/wolfviewer"

cat > "$APP/etc/wolfviewer-package" <<'EOF'
Installed from the Wolf Territories package repository. Your system's package manager
(apt, dnf, zypper or pacman) installs WolfViewer updates; the viewer does not update itself.
EOF

install -Dm755 /dev/stdin "$WORK/root/usr/bin/wolfviewer" <<'EOF'
#!/bin/sh
exec /opt/wolfviewer/wolfviewer "$@"
EOF

# Same entry refresh_desktop_app_entry.sh writes for a tarball install, at the package's paths.
install -Dm644 /dev/stdin "$WORK/root/usr/share/applications/wolfviewer.desktop" <<'EOF'
[Desktop Entry]
Name=WolfViewer
GenericName=Virtual World Viewer
Comment=Client for accessing 3D virtual worlds
Exec=/opt/wolfviewer/wolfviewer
Icon=wolfviewer
Terminal=false
Type=Application
Categories=Network;Game;
StartupNotify=true
StartupWMClass=do-not-directly-run-wolfviewer-bin
EOF
# The links. handle_secondlifeprotocol.sh hands a URL to a running viewer over D-Bus or starts one
# with -url (it runs ../wolfviewer from its own folder, so /opt works as the tarball folder does).
install -Dm644 /dev/stdin "$WORK/root/usr/share/applications/wolfviewer-url-handler.desktop" <<'EOF'
[Desktop Entry]
Name=WolfViewer (links)
Comment=Opens secondlife://, hop:// and x-grid-location-info:// links in WolfViewer
Exec=/opt/wolfviewer/etc/handle_secondlifeprotocol.sh %u
Icon=wolfviewer
Terminal=false
Type=Application
NoDisplay=true
MimeType=x-scheme-handler/secondlife;x-scheme-handler/hop;x-scheme-handler/x-grid-location-info;
EOF
install -Dm644 "$APP/wolfviewer_icon.png" "$WORK/root/usr/share/icons/hicolor/512x512/apps/wolfviewer.png"
install -Dm644 "$APP/wolfviewer_48.png"   "$WORK/root/usr/share/icons/hicolor/48x48/apps/wolfviewer.png"

# Every ELF file, split by class; and every folder in the tree that holds a shared library.
ELF64=(); ELF32=()
while IFS= read -r -d '' f; do
    case "$(file -b "$f")" in
        ELF\ 64-bit*) ELF64+=("$f") ;;
        ELF\ 32-bit*) ELF32+=("$f") ;;
    esac
done < <(find "$APP" -type f -print0)
mapfile -t LIBDIRS < <(find "$APP" -type f -name '*.so*' -printf '%h\n' | sort -u)
echo "ELF: ${#ELF64[@]} 64-bit, ${#ELF32[@]} 32-bit (left out of the dependency scan)"

POSTINST='if command -v update-desktop-database >/dev/null 2>&1; then update-desktop-database -q /usr/share/applications || true; fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true; fi'

DESC_SHORT="Wolf Territories viewer for OpenSimulator virtual worlds"
DESC_LONG="WolfViewer is the Wolf Territories Grid's viewer for OpenSimulator virtual worlds, based on the Firestorm viewer: natural water and waves, weather, terrain paint, WebRTC voice and the Wolf Territories tools."

case "$KIND" in
deb)
    PKG="$WORK/root"
    # DEBIAN/ first: dpkg-shlibdeps finds the package root (and so resolves $ORIGIN RPATHs, as
    # libcef.so has) by that folder.
    mkdir -p "$PKG/DEBIAN"
    # dpkg-shlibdeps reads debian/control from its working folder.
    mkdir -p "$WORK/shlib/debian"
    printf 'Source: wolfviewer\n\nPackage: wolfviewer\nArchitecture: amd64\n' > "$WORK/shlib/debian/control"
    LARGS=(); for d in "${LIBDIRS[@]}"; do LARGS+=("-l$d"); done
    DEPENDS="$(cd "$WORK/shlib" && dpkg-shlibdeps -O --ignore-missing-info "${LARGS[@]}" -e "${ELF64[@]}" 2>"$WORK/shlibdeps.log" \
               | sed -n 's/^shlibs:Depends=//p')" || { cat "$WORK/shlibdeps.log" >&2; exit 1; }
    grep -v "^dpkg-shlibdeps: warning: .*\(binaries to look for\|unused\|useless\)" "$WORK/shlibdeps.log" >&2 || true
    [ -n "$DEPENDS" ] || { echo "dpkg-shlibdeps produced no dependencies" >&2; exit 1; }
    SIZE_KB="$(du -sk --exclude=DEBIAN "$PKG" | cut -f1)"
    cat > "$PKG/DEBIAN/control" <<EOF
Package: wolfviewer
Version: ${VERSION}
Architecture: amd64
Maintainer: IntelligentWolf Ltd <paul@wolf.uk.com>
Section: net
Priority: optional
Homepage: https://www.wolf-grid.com/
Installed-Size: ${SIZE_KB}
Depends: ${DEPENDS}
Description: ${DESC_SHORT}
 ${DESC_LONG}
EOF
    printf '#!/bin/sh\nset -e\n%s\nexit 0\n' "$POSTINST" > "$PKG/DEBIAN/postinst"
    printf '#!/bin/sh\nset -e\n%s\nexit 0\n' "$POSTINST" > "$PKG/DEBIAN/postrm"
    chmod 755 "$PKG/DEBIAN/postinst" "$PKG/DEBIAN/postrm"
    dpkg-deb --root-owner-group -Zxz --build "$PKG" "$OUT/wolfviewer_${VERSION}_amd64.deb"
    dpkg-deb --info "$OUT/wolfviewer_${VERSION}_amd64.deb" | sed -n '/Depends/p'
    ;;
rpm)
    # Bundled sonames: never Required from the system, never Provided to it.
    mapfile -t SONAMES < <(find "$APP" -type f -name '*.so*' -printf '%f\n' | sort -u)
    REQ_EXCLUDE="^($(printf '%s\n' "${SONAMES[@]}" | sed -e 's/[.+]/\\\\&/g' | paste -sd'|'))"
    EXCL_FROM=""
    for f in "${ELF32[@]}"; do
        rel="/${f#"$WORK/root/"}"
        EXCL_FROM+="${EXCL_FROM:+|}$(printf '%s' "$rel" | sed -e 's/[.+()]/\\\\&/g')"
    done
    mkdir -p "$WORK/rpm/"{BUILD,RPMS,SOURCES,SPECS,SRPMS,BUILDROOT}
    cat > "$WORK/rpm/SPECS/wolfviewer.spec" <<EOF
%global debug_package %{nil}
%global __os_install_post %{nil}
%global __brp_mangle_shebangs %{nil}
%global _build_id_links none
%global __provides_exclude_from ^/opt/wolfviewer/.*\$
%global __requires_exclude ${REQ_EXCLUDE}
${EXCL_FROM:+%global __requires_exclude_from ^(${EXCL_FROM})\$}

Name:           wolfviewer
Version:        ${VERSION}
Release:        1
Summary:        ${DESC_SHORT}
License:        LGPL-2.1-only
URL:            https://www.wolf-grid.com/
ExclusiveArch:  x86_64

%description
${DESC_LONG}

%install
cp -a "${WORK}/root/." "%{buildroot}/"

%post
${POSTINST}

%postun
${POSTINST}

%files
/opt/wolfviewer
/usr/bin/wolfviewer
/usr/share/applications/wolfviewer.desktop
/usr/share/applications/wolfviewer-url-handler.desktop
/usr/share/icons/hicolor/512x512/apps/wolfviewer.png
/usr/share/icons/hicolor/48x48/apps/wolfviewer.png
EOF
    rpmbuild --define "_topdir $WORK/rpm" -bb "$WORK/rpm/SPECS/wolfviewer.spec" >"$WORK/rpmbuild.log" 2>&1 \
        || { tail -40 "$WORK/rpmbuild.log" >&2; exit 1; }
    RPM="$(find "$WORK/rpm/RPMS" -name 'wolfviewer-*.rpm' | head -1)"
    [ -n "$RPM" ] || { echo "rpmbuild made no package" >&2; exit 1; }
    cp "$RPM" "$OUT/"
    echo "Requires:"; rpm -qp --requires "$RPM" | grep -v '^rpmlib(' | head -80
    ;;
arch)
    mapfile -t SONAMES < <(find "$APP" -type f -name '*.so*' -printf '%f\n' | sort -u)
    declare -A BUNDLED=(); for s in "${SONAMES[@]}"; do BUNDLED["$s"]=1; done
    declare -A NEED=()
    for f in "${ELF64[@]}"; do
        while read -r so; do
            [ -n "$so" ] && [ -z "${BUNDLED[$so]:-}" ] && NEED["$so"]=1
        done < <(readelf -d "$f" 2>/dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
    done
    declare -A PKGS=(); MISSING=()
    for so in "${!NEED[@]}"; do
        owner="$(pacman -Fq "usr/lib/$so" 2>/dev/null | head -1)"
        if [ -n "$owner" ]; then PKGS["${owner##*/}"]=1; else MISSING+=("$so"); fi
    done
    [ "${#MISSING[@]}" -eq 0 ] || { echo "no Arch package provides: ${MISSING[*]}" >&2; exit 1; }
    DEPS="$(printf "'%s' " $(printf '%s\n' "${!PKGS[@]}" | sort))"
    mkdir -p "$WORK/arch"
    cat > "$WORK/arch/PKGBUILD" <<EOF
pkgname=wolfviewer
pkgver=${VERSION}
pkgrel=1
pkgdesc="${DESC_SHORT}"
arch=('x86_64')
url="https://www.wolf-grid.com/"
license=('LGPL-2.1-only')
depends=(${DEPS})
# The tree is the released build as it is: nothing stripped, rebuilt or re-linked.
options=('!strip' '!debug' '!lto' '!emptydirs')
install=wolfviewer.install
package() {
    cp -a "${WORK}/root/." "\$pkgdir/"
}
EOF
    printf 'post_install() {\n%s\n}\npost_upgrade() {\n  post_install\n}\npost_remove() {\n  post_install\n}\n' "$POSTINST" > "$WORK/arch/wolfviewer.install"
    (cd "$WORK/arch" && PKGDEST="$OUT" makepkg --nodeps --noconfirm -f >"$WORK/makepkg.log" 2>&1) \
        || { tail -40 "$WORK/makepkg.log" >&2; exit 1; }
    echo "depends: ${DEPS}"
    ;;
*)
    echo "unknown kind $KIND" >&2; exit 1 ;;
esac
ls -la "$OUT"
