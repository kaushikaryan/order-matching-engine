#pragma once
#include "Usings.h"

struct LevelInfo
{
    Price price;
    Quantity quantity;
};

using LevelInfos = std::vector<LevelInfo>;
