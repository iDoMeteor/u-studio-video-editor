#pragma once

// The test editor's built-in drop-ins (tests/dropins/meson.build): the real
// src/app/main.cpp, with the test drop-in linked in where a release build
// lists the configured ones (drop-ins/drop_ins.h.in).

#include "dropins/api.h"

#include <string>
#include <vector>

extern "C" const UStudioDropInDescription *ustudio_dropin_testdropin_describe(void);

inline std::vector<const UStudioDropInDescription *> builtinDropIns()
{
    return {
        ustudio_dropin_testdropin_describe(),
    };
}

inline std::vector<std::string> knownDropIns()
{
    return {"effects", "titles"};
}
