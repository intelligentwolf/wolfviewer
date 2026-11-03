# openh264 (vendored headers only)

Cisco OpenH264 **v2.6.0** API headers, BSD-2-Clause (licence: `LICENSE`, copied from the same tag). Downloaded
2026-11-03 from `https://raw.githubusercontent.com/cisco/openh264/v2.6.0/codec/api/wels/<file>`; byte-identical to
Arch's `openh264 2.6.0` package headers in `/usr/include/wels`. Unmodified.

    sha256 codec_api.h      21f29b20c24f7c7946f2e243d0bc2532fb3542f6c28af338209477e70d9036c9
    sha256 codec_app_def.h  a40581a24263866dca19911928f7bc4eb354ff78d9dd56dbf0f55fc4fd923726
    sha256 codec_def.h      f974d269b5935e8dc7265b8bfc02f60e5185b4d6165d30541d2758a4506f1979
    sha256 codec_ver.h      9a241e20b7c9221a5786cccd9eae3afed91afba3525b5b9b16c2101976516f94
    sha256 LICENSE          dd5c1c9668512530fa5a96e4c29ac4033d70a7eeb0eed7a42fddb6dd794ebdbb

The v2.7.0 copies of codec_api.h, codec_app_def.h and codec_def.h are byte-identical to these; v2.5.0 differs
(2.6.0 added bPsnrY/U/V to SEncParamExt and SSourcePicture and rPsnr to SLayerBSInfo), so wolfvr_openh264.cpp loads
only a 2.6.x or 2.7.x library.

`BINARY_LICENSE.txt` is Cisco's licence for its prebuilt binary (v1.0), downloaded 2026-11-03 from
`http://www.openh264.org/BINARY_LICENSE.txt` (sha256 bd9f363c5ea11ef723d0304cddacb5273c43c0e1194097c7a045d05273635418).
Condition 4 requires the viewer to reproduce it where licensing information is shown; the same text is in
`wolfvrcodecfetch.cpp` (`openh264BinaryLicenseText()`, checked byte for byte by the self-test).

No OpenH264 library is shipped with the viewer: Cisco's patent licence covers only Cisco's own binary downloaded to
the user's computer, so `wolfvrcodecfetch.cpp` fetches it on first use from `http://ciscobinary.openh264.org/` and
checks it against pinned SHA-256 values (listed there, with Cisco's published MD5s).
