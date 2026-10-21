# Third-party dependencies

cpp-httplib: v0.18.3, MIT license, used for the loopback-only native WebUI.
Source: https://github.com/yhirose/cpp-httplib/tree/v0.18.3
Header SHA-256: a0a0c13dc086663863dbe6730a19716f7d3744904b6205496e8301750814dbd5
License copy: httplib/LICENSE.

nlohmann/json single header: v3.11.3, MIT license.
Source: https://github.com/nlohmann/json/tree/v3.11.3
Header SHA-256: 9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
License copy: nlohmann/LICENSE.MIT.

OpenSSL libcrypto is discovered by CMake and is not vendored.
WinDivert ABI header: official v2.2.2, unmodified, dual LGPLv3/GPLv2 licensing.
Source: https://github.com/basil00/WinDivert/blob/v2.2.2/include/windivert.h
License copy: windivert/LICENSE. The program loads the user-selected DLL dynamically.
The repository's existing runtime/release_resources DLL/driver pair is reused;
the test-only mock DLL is independently implemented and never installed as a driver.
The optional Python reference bridge requires the existing recovered CPython 3.14
modules and NumPy; they are not presented as newly reconstructed C++.
