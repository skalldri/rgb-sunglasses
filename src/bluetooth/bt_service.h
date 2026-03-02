#pragma once

#include <cstddef>

enum class BtServiceId
{
    Invalid = 0,
    CoreConfig = 1, // Configuration of the entire HMD
    ZigZag = 2,
    Text = 3,
    Rainbow = 4,
    MyEyes = 5,
};

template <BtServiceId tBtServiceId>
class BtService
{
public:
    static constexpr BtServiceId kBtServiceId = tBtServiceId;
    static constexpr size_t kBtServiceIdNum = (size_t)tBtServiceId;
};
