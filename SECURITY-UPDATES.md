# WolfViewer security updates — 7.2.4 w48 "Fanatical Frog"

This release replaces most of the third-party libraries WolfViewer inherited from the
Firestorm / Second Life viewer build with current, patched versions, and fixes several
issues in the viewer's own code. Everything below is in the public source; the library
packages are built by GitHub Actions from the public repositories listed at the end.

## Third-party libraries

| Library | Was | Now | Why it matters |
|---|---|---|---|
| curl (all HTTP/HTTPS traffic) | 7.54.1 (2017) | **8.22.0** | curl lists 82 published vulnerabilities affecting 7.54.1 ([curl.se/docs/vuln-7.54.1.html](https://curl.se/docs/vuln-7.54.1.html)). Now built with HTTP and HTTPS only — no FTP, TFTP, SMTP or other protocol code is compiled in. |
| OpenSSL (TLS for all HTTPS) | 1.1.1w (end of life since September 2023) | **3.5.8 LTS** | 1.1.1 receives no public security fixes; e.g. CVE-2026-45447 (High). |
| nghttp2 (HTTP/2) | 1.64.0 | **1.70.0** | CVE-2026-27135 (High). |
| expat (XML / LLSD parsing, including data from other grids) | 2.6.4 | **2.8.5** | e.g. CVE-2024-8176 (High), CVE-2025-59375 (High), and the further fixes listed in expat's [Changes](https://github.com/libexpat/libexpat/blob/R_2_8_5/expat/Changes). |
| libpng | 1.6.44 | **1.6.58** | e.g. CVE-2025-64720, CVE-2025-66293, CVE-2026-25646 (all High). |
| OpenJPEG (decodes every texture) | 2.5.3 | **2.5.4** + upstream fix | CVE-2025-54874 (Critical); the not-yet-released upstream fix for CVE-2026-6192 is applied. |
| FreeType (fonts) | 2.13.3 | **2.14.3** + upstream fix | CVE-2026-23865; the not-yet-released upstream fix for CVE-2026-50811 is applied. |
| libxml2 (mesh .dae import) | 2.13.5 | **2.15.4** | e.g. CVE-2025-24928, CVE-2025-6021 (High). |
| VLC (Windows/macOS media streams) | 3.0.21 | **3.0.24** | e.g. CVE-2025-51602; files are VideoLAN's official 3.0.24 builds, checksums verified. |
| libwebrtc (voice) | m137 | **m144** | Current Chromium WebRTC (Second Life's m144 build). |
| zlib-ng, minizip-ng, colladadom, APR, Boost, meshoptimizer | various | current | Rebuilt against the updated libraries above. |

## Fixes in WolfViewer's own code

- **Redirects restricted to HTTP/HTTPS.** Every web request the viewer makes now refuses to
  follow a redirect into any other protocol (before, a server could redirect the viewer into
  curl's FTP/TFTP code).
- **Material (PBR) assets can no longer make the viewer open files or decode embedded
  images.** A material created by another user could name a local file path — or on Windows a
  `\\server\share` path, which leaks the user's network credentials to another machine — as an
  "image". Materials only need texture IDs, so file access and image decoding are now off for
  material data received from the network.
- **Saved logins keep working on OpenSSL 3.** The saved-credential store uses RC4, which
  OpenSSL 3 only provides through its "legacy" provider; it is now loaded explicitly.

## Known and deliberately deferred

- **Built-in web browser (CEF 152 / Chromium 152).** Newer Chromium releases fix further
  vulnerabilities. Moving to CEF 154 with media codecs needs a from-source Chromium build and
  is planned for the next release.
- **TLS host-name checking.** Like other Second Life-derived viewers, only some requests ask
  curl to check the certificate's host name; the rest rely on the viewer's own certificate
  handling, which many OpenSim grids with self-signed certificates depend on. Changing this
  needs care not to lock users out of grids.
- **Local-only components** (only reachable from your own files or desktop, not from other
  users or grids): glib 2.64 and pcre 8.35 (Linux), nanosvg, and APR-util functions the viewer
  never calls.

## Package sources

Built by GitHub Actions from: intelligentwolf/3p-curl, 3p-openssl, 3p-nghttp2, 3p-expat,
3p-libpng, 3p-openjpeg, 3p-freetype, 3p-libxml2, 3p-colladadom, 3p-apr_suite, 3p-vlc-bin
(forks of Second Life's package repositories). Other packages are Second Life's own releases.
