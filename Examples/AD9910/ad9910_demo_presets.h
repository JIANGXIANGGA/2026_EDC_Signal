#ifndef AD9910_DEMO_PRESETS_H
#define AD9910_DEMO_PRESETS_H

#include "ad9910_demo_app.h"

ad9910_demo_preset_id_t AD9910_Demo_Presets_GetBootPreset(void);
const ad9910_demo_config_t *AD9910_Demo_Presets_Get(
    ad9910_demo_preset_id_t preset);

#endif
