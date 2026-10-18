#pragma once
// The vocabulary of upscaling methods, shared by everything that has to name
// one: the command-line benchmark (src/upscale.cpp), the launch-options writer
// (src/steam.cpp) and the GUI.
//
// Header-only on purpose. The Steam-side code must not pull in Vulkan and the
// benchmark must not pull in the Steam library, but the launch options string
// the GUI writes is the *same* string the layer reads, so all three have to
// agree on the names. One table, included by all of them, is the only way that
// cannot drift.
//
// "auto" is deliberately not in the table. It is a policy -- the layer picks a
// method at runtime -- rather than a filter, so it has no CPU reference and the
// benchmark cannot be asked to run it. The layer and the GUI special-case it
// (see lsl_auto_pick() in layer/lsl_upscale.c and isAutoMethod() below).

#include <cstddef>
#include <string>

namespace lsl {

enum ScaleMode : unsigned {
    MODE_NEAREST  = 0,
    MODE_BILINEAR = 1,
    MODE_CUBIC    = 2,  // Catmull-Rom (Mitchell B=0, C=0.5)
    MODE_LANCZOS2 = 3,
    MODE_FSR1     = 4,  // AMD FidelityFX Super Resolution 1 (EASU), MIT
    MODE_FSR1RCAS = 5,  // EASU followed by FSR1's RCAS sharpening pass
    // --- added later, and appended rather than inserted: the layer sends the
    //     numeric id to the shader in its push constant, and a swapchain
    //     already upscaled with mode 4 must keep meaning EASU.
    MODE_LANCZOS3 = 6,  // Lanczos-3, a wider window than Lanczos-2
    MODE_NIS      = 7,  // EASU, then NVIDIA Image Scaling's sharpening
    MODE_CNN      = 8,  // EASU, then Anime4K v4's Restore CNN
    MODE_COUNT    = 9,
};

struct MethodInfo {
    const char* name;   // the id: what LSL_LAYER_MODE= and --modes take
    const char* label;  // what the GUI shows
};

// The one line that decides what a method is called. Indexed by ScaleMode, so
// MODE_COUNT entries exactly.
inline const MethodInfo* methodTable() {
    static const MethodInfo kTable[MODE_COUNT] = {
        {"nearest",  "Nearest"},
        {"bilinear", "Bilinear"},
        {"cubic",    "Bicubic"},
        {"lanczos2", "Lanczos-2"},
        {"fsr1",     "FSR1 (EASU)"},
        {"fsr1rcas", "FSR1 (EASU + RCAS)"},
        {"lanczos3", "Lanczos-3"},
        {"nis",      "NIS (EASU + NIS sharpen)"},
        {"cnn",      "AI (EASU + Anime4K CNN)"},
    };
    return kTable;
}

inline const char* modeName(unsigned mode) {
    return mode < MODE_COUNT ? methodTable()[mode].name : "unknown";
}

inline const char* modeLabel(unsigned mode) {
    return mode < MODE_COUNT ? methodTable()[mode].label : "unknown";
}

// The method whose `name` is exactly `n`, or -1 if there is none. Meant for
// hand-typed launch options and --modes: a bad name is a normal thing to get,
// not an error, so it answers with -1 rather than throwing.
inline int methodIdFromName(const char* n) {
    if (!n || !*n) return -1;
    // canonical aliases
    const char* b = n;
    if (b[0] == 'f' && b[1] == 's' && b[2] == 'r' && b[3] == '\0') return MODE_FSR1RCAS;
    for (unsigned m = 0; m < MODE_COUNT; ++m) {
        const char* a = methodTable()[m].name;
        b = n;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') return int(m);
    }
    return -1;
}

// Whether a launch-options value asks the layer to choose. Written by the GUI
// as LSL_LAYER_MODE=auto; anything else -- including an empty string -- is a
// concrete method, with the classical default (fsr1rcas) when it is unknown.
inline bool isAutoMethod(const char* n) {
    if (!n) return false;
    const char* a = "auto";
    while (*a && *a == *n) { ++a; ++n; }
    return *a == '\0' && *n == '\0';
}

// Whether `m` is a LSL_LAYER_MODE= value at all: a real method, or "auto".
// The launch-options writer and the profile store both refuse everything else,
// so a typo in the GUI's text entry or a hand-edited profile cannot reach the
// layer as a name it will only warn about.
inline bool isModeName(const std::string& m) {
    return isAutoMethod(m.c_str()) || methodIdFromName(m.c_str()) >= 0;
}

}  // namespace lsl
