#pragma once
#include <vector>
#include "Common/CommonTypes.h"

struct GMORange { u32 start; u32 end; };
extern std::vector<GMORange> g_gmoRanges;
extern bool g_isoTexDumpActive;
