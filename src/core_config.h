#pragma once

#include <singleton.h>
#include <bluetooth/bt_service.h>

class CoreConfig : public Singleton<CoreConfig>, public BtService<BtServiceId::CoreConfig> {
    public:
    /**
     * @brief Returns a value between 0 and 1 representing the current display brightness
     * 
     * @return float 
     */
    float getBrightnessFactor();

    float getDisplayRateMs();

    float getRenderRateMs();
};