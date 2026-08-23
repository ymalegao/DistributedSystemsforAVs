#pragma once

#include <omnetpp.h>

// V2VBFT_API macro. Lets the same headers serve both building the shared
// library and linking against it. Mirrors Veins' VEINS_API; the project
// prefix is set by opp_makemake's -p V2VBFT.
#if defined(V2VBFT_EXPORT)
#define V2VBFT_API OPP_DLLEXPORT
#elif defined(V2VBFT_IMPORT)
#define V2VBFT_API OPP_DLLIMPORT
#else
#define V2VBFT_API
#endif
