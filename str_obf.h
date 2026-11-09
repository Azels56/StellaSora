#pragma once
// Compile-time string masking.
//
// The payload used to carry every path and every game class name as plaintext
// in .rdata, which identifies it instantly under a hex editor or a string scan.
// A constexpr Enc is evaluated at compile time, so only the masked bytes are
// emitted; the literal itself never reaches the binary.
#include <cstddef>

namespace obf {

constexpr char kMask = 0x5A;

template <size_t N>
struct Enc {
    char data[N];

    constexpr Enc(const char* s) : data{} {
        for (size_t i = 0; i + 1 < N && s[i]; ++i)
            data[i] = static_cast<char>(s[i] ^ kMask);
    }
};

// Decodes into its own storage, so it can be passed straight to a function that
// takes a const char*: the temporary outlives the call.
template <size_t N>
struct Decoded {
    char buf[N];

    Decoded(const Enc<N>& e) {
        for (size_t i = 0; i < N; ++i) buf[i] = static_cast<char>(e.data[i] ^ kMask);
        buf[N - 1] = 0;
    }
    operator const char*() const { return buf; }
    const char* c_str() const { return buf; }
};

} // namespace obf

// Usage:  static constexpr auto kPath = OBF("C:\\...");
//         fopen_s(&f, obf::Decoded(kPath), "a");
#define OBF(s) (::obf::Enc<sizeof(s)>(s))
