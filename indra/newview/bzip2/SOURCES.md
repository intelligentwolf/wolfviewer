# bzip2 (vendored)

bzip2 / libbzip2 **1.0.8** by Julian Seward (BSD-style licence: `LICENSE`, from the same tarball). Used only to
unpack Cisco's OpenH264 download (`.bz2`) in `wolfvrcodecfetch.cpp`.

Source: `https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz`, downloaded 2026-11-03.

    sha256 bzip2-1.0.8.tar.gz ab5a03176ee106d3f0fa90e381da478ddae405918153cca248e682cd0c4a2269
    sha512 bzip2-1.0.8.tar.gz 083f5e675d73f3233c7930ebe20425a533feedeaaa9d8cc86831312a6581cefbe6ed0d08d2fa89be81082f2a5abdabca8b3c080bf97218a1bd59dc118a30b9f3

The sha512 matches the line for bzip2-1.0.8.tar.gz in sourceware's published list,
`https://sourceware.org/pub/bzip2/sha512.sum` (sourceware publishes SHA-512 there, not SHA-256; the sha256 above was
computed from the same verified file). The library files were copied from the tarball unmodified:

    sha256 blocksort.c      4e48cd2ccff44699e67a7c949b0e9576c05b8dcbe20f863475c4fcc8db11a409
    sha256 bzlib.c          d06cf1bd991df1f2dc8ef4f7713d186eb636767111cbd4807ef5fc4a54ca6838
    sha256 bzlib.h          6ac62e811669598ee30c9e1c379b9e627f6ff17a5a3dc1e0b4fa8b8ea75e580d
    sha256 bzlib_private.h  c0cda4f35ee1f2d54c9beacd524f8d28e0dbf8494aca30d854af3f143af4341b
    sha256 compress.c       75995bd6e8c5f1e1dad05178f3cf53137df99ce860a1984324f78591f28deed3
    sha256 crctable.c       2fb7a564629386456e731f431a5cf4f5026747bace4cd10be8f5ecf082066a92
    sha256 decompress.c     31a89f8bf408ef0e4acae83e8be60a8eb4edece6c866d6e32b8f7e557ca54bc6
    sha256 huffman.c        bdeb45f3f535546a672811b68aa87cc58fd395b28ecebc34fa3566a656a4d1d1
    sha256 randtable.c      407054ca6f54cd737dbc26ceb6b7874b55a0fcff86c2eb23cbec2fbdbb884815
    sha256 LICENSE          c6dbbf828498be844a89eaa3b84adbab3199e342eb5cb2ed2f0d4ba7ec0f38a3

The compression files (blocksort.c, compress.c, huffman.c) are not called, but bzlib.c refers to them, so all seven
are built. It is plain C and not valid C++, so indra/newview/CMakeLists.txt builds it as its own static C library
(`wolfvr_bzip2`) with the project's /TP, /WX and -Werror removed for that target only.
