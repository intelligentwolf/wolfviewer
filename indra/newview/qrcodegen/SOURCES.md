# qrcodegen (vendored)

Nayuki's QR Code generator library, C++ version, tag v1.8.0 (MIT licence; the licence text is at the top of both
files). Downloaded 2026-11-03 from
https://raw.githubusercontent.com/nayuki/QR-Code-generator/v1.8.0/cpp/qrcodegen.{cpp,hpp}

    sha256 qrcodegen.cpp 1f3b3fcdac6954c32cf583ccd02ec9b5901f756a38c461acedc70be4a77d3757
    sha256 qrcodegen.hpp b779c3b156cf7a57ce789d6fee4fc991ccc2913774d26c909d22bb8f26b2a793

qrcodegen.hpp is byte-identical to Arch's qrcodegencpp-cmake 1.8.0 header. Unmodified. Used by the VR headset
floater (wolffloatervrstream.cpp) to draw the address as a QR code, and by tests/wolfvrpipe_selftest.cpp.
