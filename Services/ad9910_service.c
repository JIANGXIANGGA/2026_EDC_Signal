#include "ad9910_service.h"

#include "ad9910.h"
#include "main.h"

#define AD9910_SERVICE_POWER_UP_DELAY_MS 300U
#define AD9910_SERVICE_PLL_LOCK_DELAY_MS 10U
#define AD9910_SERVICE_DEFAULT_FREQUENCY_HZ 1000U
#define AD9910_SERVICE_DEFAULT_AMPLITUDE AD9910_MAX_AMPLITUDE
#define AD9910_SERVICE_DEFAULT_PHASE_OFFSET 0U

/* PC6/PC7/PC8 是本板 AD9910 数字斜坡控制专用引脚。 */
#define AD9910_SERVICE_DRCTL_GPIO_PORT GPIOC
#define AD9910_SERVICE_DRCTL_GPIO_PIN GPIO_PIN_6
#define AD9910_SERVICE_DRHOLD_GPIO_PORT GPIOC
#define AD9910_SERVICE_DRHOLD_GPIO_PIN GPIO_PIN_7
#define AD9910_SERVICE_DROVER_GPIO_PORT GPIOC
#define AD9910_SERVICE_DROVER_GPIO_PIN GPIO_PIN_8
#define AD9910_SERVICE_DROVER_EXTI_IRQn EXTI9_5_IRQn

/* 以下 CFR 值来自 KV-AD9910 配套示例：40 MHz 参考输入，PLL 倍频到 1 GHz。 */
static const uint8_t g_ad9910_cfr1[AD9910_CFR_DATA_LENGTH] = {
    0x00U, 0x40U, 0x00U, 0x00U
};
static const uint8_t g_ad9910_fixed_cfr2[AD9910_CFR_DATA_LENGTH] = {
    0x01U, 0x00U, 0x00U, 0x00U
};
/* 使能频率 DRG；关闭自动换向，由 DRCTL 控制上、下扫。 */
static const uint8_t g_ad9910_manual_sweep_cfr2[AD9910_CFR_DATA_LENGTH] = {
    0x01U, 0x48U, 0x08U, 0x20U
};
static const uint8_t g_ad9910_cfr3[AD9910_CFR_DATA_LENGTH] = {
    0x05U, 0x0FU, 0x41U, 0x32U
};

typedef struct {
    ad9910_t device;
    ad9910_service_state_t state;
    uint32_t deadline_ms;
    uint32_t frequency_hz;
    uint16_t amplitude;
    uint16_t phase_offset;
    ad9910_tone_config_t profiles[AD9910_PROFILE_COUNT];
    uint8_t profile_valid_mask;
    uint8_t selected_profile;
    uint8_t profile_write_index;
    uint8_t profile0[AD9910_PROFILE_DATA_LENGTH];
    uint8_t ram_profile[AD9910_PROFILE_DATA_LENGTH];
    uint8_t base_ftw[AD9910_FTW_DATA_LENGTH];
    uint8_t base_pow[AD9910_POW_DATA_LENGTH];
    uint8_t base_asf[AD9910_ASF_DATA_LENGTH];
    uint8_t ram_cfr1[AD9910_CFR_DATA_LENGTH];
    uint8_t ram_data[AD9910_RAM_DATA_LENGTH];
    uint16_t ram_data_length;
    uint8_t digital_ramp_limits[AD9910_DRL_DATA_LENGTH];
    uint8_t digital_ramp_steps[AD9910_DRS_DATA_LENGTH];
    uint8_t digital_ramp_rates[AD9910_DRR_DATA_LENGTH];
    uint8_t profile_update_requested;
    uint8_t sweep_start_requested;
    uint8_t sweep_stop_requested;
    uint8_t sweep_stop_in_progress;
    uint8_t sweep_active;
    uint8_t sweep_direction_up;
    uint8_t sweep_hold;
    uint8_t ram_start_requested;
    uint8_t ram_stop_requested;
    uint8_t ram_active;
    ad9910_ram_destination_t ram_destination;
    ad9910_ram_mode_t ram_mode;
    uint16_t ram_sample_count;
    uint16_t ram_address_step_rate;
    volatile uint32_t ramp_limit_event_count;
    uint32_t last_hal_error;
} ad9910_service_context_t;

static ad9910_service_context_t g_ad9910_service;

static uint8_t ad9910_service_time_expired(uint32_t deadline_ms)
{
    return ((int32_t)(HAL_GetTick() - deadline_ms) >= 0) ? 1U : 0U;
}

static void ad9910_service_init_ramp_gpio(void)
{
    GPIO_InitTypeDef gpio_init = {0};

    /* RST 已在模块侧接地，释放旧 CubeMX 生成代码暂时占用的 PE5。 */
    HAL_GPIO_DeInit(GPIOE, GPIO_PIN_5);

    __HAL_RCC_GPIOC_CLK_ENABLE();

    HAL_GPIO_WritePin(AD9910_SERVICE_DRCTL_GPIO_PORT,
                      AD9910_SERVICE_DRCTL_GPIO_PIN |
                          AD9910_SERVICE_DRHOLD_GPIO_PIN,
                      GPIO_PIN_RESET);

    gpio_init.Pin = AD9910_SERVICE_DRCTL_GPIO_PIN |
                    AD9910_SERVICE_DRHOLD_GPIO_PIN;
    gpio_init.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_init.Pull = GPIO_NOPULL;
    gpio_init.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOC, &gpio_init);

    gpio_init.Pin = AD9910_SERVICE_DROVER_GPIO_PIN;
    gpio_init.Mode = GPIO_MODE_IT_RISING;
    gpio_init.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(AD9910_SERVICE_DROVER_GPIO_PORT, &gpio_init);

    HAL_NVIC_SetPriority(AD9910_SERVICE_DROVER_EXTI_IRQn, 6U, 0U);
    HAL_NVIC_EnableIRQ(AD9910_SERVICE_DROVER_EXTI_IRQn);
}

static void ad9910_service_set_sweep_direction(uint8_t direction_up)
{
    g_ad9910_service.sweep_direction_up = (direction_up != 0U) ? 1U : 0U;
    HAL_GPIO_WritePin(AD9910_SERVICE_DRCTL_GPIO_PORT,
                      AD9910_SERVICE_DRCTL_GPIO_PIN,
                      (g_ad9910_service.sweep_direction_up != 0U) ?
                          GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void ad9910_service_set_sweep_hold(uint8_t hold)
{
    g_ad9910_service.sweep_hold = (hold != 0U) ? 1U : 0U;
    HAL_GPIO_WritePin(AD9910_SERVICE_DRHOLD_GPIO_PORT,
                      AD9910_SERVICE_DRHOLD_GPIO_PIN,
                      (g_ad9910_service.sweep_hold != 0U) ?
                          GPIO_PIN_SET : GPIO_PIN_RESET);
}

static uint8_t ad9910_service_tone_config_is_valid(
    const ad9910_tone_config_t *config)
{
    return (config != NULL) &&
           (config->frequency_hz <= AD9910_MAX_FREQUENCY_HZ) &&
           (config->amplitude <= AD9910_MAX_AMPLITUDE);
}

static void ad9910_service_build_profile(const ad9910_tone_config_t *config) {
    AD9910_BuildProfile0(g_ad9910_service.profile0,
                         config->amplitude,
                         config->phase_offset,
                         AD9910_FrequencyToFTW(config->frequency_hz));
}

static void ad9910_service_select_profile_gpio(uint8_t profile_index)
{
    uint16_t set_pins = 0U;
    const uint16_t profile_pins = AD9910_PROFILE0_Pin |
                                  AD9910_PROFILE1_Pin |
                                  AD9910_PROFILE2_Pin;

    if ((profile_index & 0x01U) != 0U) {
        set_pins |= AD9910_PROFILE0_Pin;
    }
    if ((profile_index & 0x02U) != 0U) {
        set_pins |= AD9910_PROFILE1_Pin;
    }
    if ((profile_index & 0x04U) != 0U) {
        set_pins |= AD9910_PROFILE2_Pin;
    }

    HAL_GPIO_WritePin(AD9910_PROFILE0_GPIO_Port, profile_pins, GPIO_PIN_RESET);
    if (set_pins != 0U) {
        HAL_GPIO_WritePin(AD9910_PROFILE0_GPIO_Port, set_pins, GPIO_PIN_SET);
    }
}

static void ad9910_service_update_selected_profile_status(void)
{
    const ad9910_tone_config_t *profile =
        &g_ad9910_service.profiles[g_ad9910_service.selected_profile];

    g_ad9910_service.frequency_hz = profile->frequency_hz;
    g_ad9910_service.amplitude = profile->amplitude;
    g_ad9910_service.phase_offset = profile->phase_offset;
}

static uint8_t ad9910_service_sweep_config_is_valid(
    const ad9910_frequency_sweep_config_t *config)
{
    if (config == NULL) {
        return 0U;
    }

    return (config->lower_frequency_hz < config->upper_frequency_hz) &&
           (config->upper_frequency_hz <= AD9910_MAX_FREQUENCY_HZ) &&
           (config->positive_step_hz > 0U) &&
           (config->negative_step_hz > 0U) &&
           (config->positive_step_hz <= AD9910_MAX_FREQUENCY_HZ) &&
           (config->negative_step_hz <= AD9910_MAX_FREQUENCY_HZ) &&
           (config->positive_rate > 0U) &&
           (config->negative_rate > 0U);
}

static void ad9910_service_build_sweep_registers(
    const ad9910_frequency_sweep_config_t *config)
{
    AD9910_BuildDigitalRampLimits(
        g_ad9910_service.digital_ramp_limits,
        AD9910_FrequencyToFTW(config->lower_frequency_hz),
        AD9910_FrequencyToFTW(config->upper_frequency_hz));
    AD9910_BuildDigitalRampSteps(
        g_ad9910_service.digital_ramp_steps,
        AD9910_FrequencyToFTW(config->positive_step_hz),
        AD9910_FrequencyToFTW(config->negative_step_hz));
    AD9910_BuildDigitalRampRates(g_ad9910_service.digital_ramp_rates,
                                 config->positive_rate,
                                 config->negative_rate);
}

static uint8_t ad9910_service_ram_config_is_valid(
    const ad9910_ram_playback_config_t *config)
{
    uint16_t address_step_rate;

    if ((config == NULL) ||
        (config->profile_index >= AD9910_PROFILE_COUNT) ||
        (config->sample_count == 0U) ||
        (config->ram_words == NULL) ||
        (config->ram_word_count != config->sample_count) ||
        (config->start_address >= AD9910_RAM_WORD_COUNT) ||
        (config->sample_count > AD9910_RAM_WORD_COUNT) ||
        (((uint32_t)config->start_address + config->sample_count) >
            AD9910_RAM_WORD_COUNT) ||
        (config->destination > AD9910_RAM_DESTINATION_POLAR) ||
        (config->mode > AD9910_RAM_MODE_CONTINUOUS_RECIRCULATE) ||
        (ad9910_service_tone_config_is_valid(&config->base_tone) == 0U)) {
        return 0U;
    }

    address_step_rate = config->address_step_rate;
    if (address_step_rate == 0U) {
        address_step_rate =
            AD9910_RamPlaybackRateToStepRate(config->playback_sample_rate_hz);
    }

    return (address_step_rate > 0U) ? 1U : 0U;
}

static void ad9910_service_build_ram_registers(
    const ad9910_ram_playback_config_t *config)
{
    uint16_t address_step_rate = config->address_step_rate;
    const uint16_t end_address =
        (uint16_t)(config->start_address + config->sample_count - 1U);

    if (address_step_rate == 0U) {
        address_step_rate =
            AD9910_RamPlaybackRateToStepRate(config->playback_sample_rate_hz);
    }

    AD9910_BuildFTW(g_ad9910_service.base_ftw,
                    AD9910_FrequencyToFTW(config->base_tone.frequency_hz));
    AD9910_BuildPOW(g_ad9910_service.base_pow,
                    config->base_tone.phase_offset);
    AD9910_BuildASF(g_ad9910_service.base_asf,
                    config->base_tone.amplitude);
    AD9910_BuildRamProfile(g_ad9910_service.ram_profile,
                           config->start_address,
                           end_address,
                           address_step_rate,
                           config->mode,
                           config->no_dwell_high,
                           config->zero_crossing);
    AD9910_BuildCFR1RamPlayback(g_ad9910_service.ram_cfr1,
                                g_ad9910_cfr1,
                                config->destination);
    AD9910_PackRamWords(g_ad9910_service.ram_data,
                        config->ram_words,
                        config->ram_word_count);

    g_ad9910_service.ram_data_length =
        (uint16_t)(config->ram_word_count * AD9910_RAM_WORD_SIZE);
    g_ad9910_service.profile_write_index = config->profile_index;
    g_ad9910_service.selected_profile = config->profile_index;
    g_ad9910_service.profile_valid_mask |=
        (uint8_t)(1U << config->profile_index);
    g_ad9910_service.ram_destination = config->destination;
    g_ad9910_service.ram_mode = config->mode;
    g_ad9910_service.ram_sample_count = config->sample_count;
    g_ad9910_service.ram_address_step_rate = address_step_rate;
    g_ad9910_service.frequency_hz = config->base_tone.frequency_hz;
    g_ad9910_service.amplitude = config->base_tone.amplitude;
    g_ad9910_service.phase_offset = config->base_tone.phase_offset;
    g_ad9910_service.profiles[config->profile_index] = config->base_tone;
    ad9910_service_build_profile(&config->base_tone);
}

static void ad9910_service_set_error(void)
{
    g_ad9910_service.last_hal_error =
        AD9910_GetLastHalError(&g_ad9910_service.device);
    g_ad9910_service.state = AD9910_SERVICE_STATE_ERROR;
}

static HAL_StatusTypeDef ad9910_service_write_register(uint8_t address,
                                                        const uint8_t *data,
                                                        uint16_t length)
{
    HAL_StatusTypeDef status;

    status = AD9910_WriteRegisterDMA(&g_ad9910_service.device,
                                     address,
                                     data,
                                     length);
    if ((status != HAL_OK) && (status != HAL_BUSY)) {
        ad9910_service_set_error();
    }

    return status;
}

HAL_StatusTypeDef AD9910_Service_Init(SPI_HandleTypeDef *spi)
{
    const ad9910_pin_config_t pins = {
        .csb_port = AD9910_CSB_GPIO_Port,
        .csb_pin = AD9910_CSB_Pin,
        .io_update_port = AD9910_IO_UPDATE_GPIO_Port,
        .io_update_pin = AD9910_IO_UPDATE_Pin,
    };
    HAL_StatusTypeDef status;

    status = AD9910_Init(&g_ad9910_service.device, spi, &pins);
    if (status != HAL_OK) {
        return status;
    }

    g_ad9910_service.frequency_hz = AD9910_SERVICE_DEFAULT_FREQUENCY_HZ;
    g_ad9910_service.amplitude = AD9910_SERVICE_DEFAULT_AMPLITUDE;
    g_ad9910_service.phase_offset = AD9910_SERVICE_DEFAULT_PHASE_OFFSET;
    g_ad9910_service.profiles[0].frequency_hz =
        AD9910_SERVICE_DEFAULT_FREQUENCY_HZ;
    g_ad9910_service.profiles[0].amplitude = AD9910_SERVICE_DEFAULT_AMPLITUDE;
    g_ad9910_service.profiles[0].phase_offset =
        AD9910_SERVICE_DEFAULT_PHASE_OFFSET;
    g_ad9910_service.profile_valid_mask = 0x01U;
    g_ad9910_service.selected_profile = 0U;
    g_ad9910_service.profile_write_index = 0U;
    g_ad9910_service.profile_update_requested = 0U;
    g_ad9910_service.sweep_start_requested = 0U;
    g_ad9910_service.sweep_stop_requested = 0U;
    g_ad9910_service.sweep_stop_in_progress = 0U;
    g_ad9910_service.sweep_active = 0U;
    g_ad9910_service.ram_start_requested = 0U;
    g_ad9910_service.ram_stop_requested = 0U;
    g_ad9910_service.ram_active = 0U;
    g_ad9910_service.ram_destination = AD9910_RAM_DESTINATION_FREQUENCY;
    g_ad9910_service.ram_mode = AD9910_RAM_MODE_DIRECT_SWITCH;
    g_ad9910_service.ram_sample_count = 0U;
    g_ad9910_service.ram_address_step_rate = 0U;
    g_ad9910_service.ram_data_length = 0U;
    g_ad9910_service.ramp_limit_event_count = 0U;
    g_ad9910_service.last_hal_error = HAL_SPI_ERROR_NONE;
    ad9910_service_init_ramp_gpio();
    ad9910_service_set_sweep_direction(1U);
    ad9910_service_set_sweep_hold(0U);
    ad9910_service_select_profile_gpio(0U);
    ad9910_service_build_profile(&g_ad9910_service.profiles[0]);

    /* PWR、RST 均已在模块侧接地，仅等待独立 5 V 电源稳定。 */
    g_ad9910_service.deadline_ms = HAL_GetTick() + AD9910_SERVICE_POWER_UP_DELAY_MS;
    g_ad9910_service.state = AD9910_SERVICE_STATE_POWER_UP_DELAY;

    return HAL_OK;
}

void AD9910_Service_Process(void)
{
    ad9910_transfer_event_t event;
    HAL_StatusTypeDef status;

    event = AD9910_Process(&g_ad9910_service.device);
    if (event == AD9910_TRANSFER_EVENT_ERROR) {
        ad9910_service_set_error();
        return;
    }

    switch (g_ad9910_service.state) {
    case AD9910_SERVICE_STATE_POWER_UP_DELAY:
        if (ad9910_service_time_expired(g_ad9910_service.deadline_ms) != 0U) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_CFR1;
        }
        break;

    case AD9910_SERVICE_STATE_WRITE_CFR1:
        status = ad9910_service_write_register(AD9910_REGISTER_CFR1,
                                                g_ad9910_cfr1,
                                                AD9910_CFR_DATA_LENGTH);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_CFR1;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_CFR1:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_CFR2;
        }
        break;

    case AD9910_SERVICE_STATE_WRITE_CFR2:
        status = ad9910_service_write_register(AD9910_REGISTER_CFR2,
                                                g_ad9910_fixed_cfr2,
                                                AD9910_CFR_DATA_LENGTH);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_CFR2;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_CFR2:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_CFR3;
        }
        break;

    case AD9910_SERVICE_STATE_WRITE_CFR3:
        status = ad9910_service_write_register(AD9910_REGISTER_CFR3,
                                                g_ad9910_cfr3,
                                                AD9910_CFR_DATA_LENGTH);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_CFR3;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_CFR3:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_APPLY_CFR;
        }
        break;

    case AD9910_SERVICE_STATE_APPLY_CFR:
        if (AD9910_IOUpdate(&g_ad9910_service.device) != HAL_OK) {
            ad9910_service_set_error();
            break;
        }
        g_ad9910_service.deadline_ms = HAL_GetTick() + AD9910_SERVICE_PLL_LOCK_DELAY_MS;
        g_ad9910_service.state = AD9910_SERVICE_STATE_PLL_LOCK_WAIT;
        break;

    case AD9910_SERVICE_STATE_PLL_LOCK_WAIT:
        if (ad9910_service_time_expired(g_ad9910_service.deadline_ms) != 0U) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_PROFILE0;
        }
        break;

    case AD9910_SERVICE_STATE_WRITE_PROFILE0:
        status = ad9910_service_write_register(
                                                (uint8_t)(AD9910_REGISTER_PROFILE0 +
                                                    g_ad9910_service.profile_write_index),
                                                g_ad9910_service.profile0,
                                                AD9910_PROFILE_DATA_LENGTH);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_PROFILE0;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_PROFILE0:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_APPLY_PROFILE0;
        }
        break;

    case AD9910_SERVICE_STATE_APPLY_PROFILE0:
        if (AD9910_IOUpdate(&g_ad9910_service.device) != HAL_OK) {
            ad9910_service_set_error();
            break;
        }
        if (g_ad9910_service.sweep_stop_in_progress != 0U) {
            g_ad9910_service.sweep_stop_in_progress = 0U;
            g_ad9910_service.sweep_active = 0U;
            ad9910_service_set_sweep_hold(0U);
        }
        g_ad9910_service.state = AD9910_SERVICE_STATE_READY;
        break;

    case AD9910_SERVICE_STATE_WRITE_SWEEP_CFR2:
        status = ad9910_service_write_register(AD9910_REGISTER_CFR2,
                                                g_ad9910_manual_sweep_cfr2,
                                                AD9910_CFR_DATA_LENGTH);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_SWEEP_CFR2;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_SWEEP_CFR2:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_DRL;
        }
        break;

    case AD9910_SERVICE_STATE_WRITE_DRL:
        status = ad9910_service_write_register(
            AD9910_REGISTER_DRL,
            g_ad9910_service.digital_ramp_limits,
            AD9910_DRL_DATA_LENGTH);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_DRL;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_DRL:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_DRS;
        }
        break;

    case AD9910_SERVICE_STATE_WRITE_DRS:
        status = ad9910_service_write_register(
            AD9910_REGISTER_DRS,
            g_ad9910_service.digital_ramp_steps,
            AD9910_DRS_DATA_LENGTH);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_DRS;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_DRS:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_DRR;
        }
        break;

    case AD9910_SERVICE_STATE_WRITE_DRR:
        status = ad9910_service_write_register(
            AD9910_REGISTER_DRR,
            g_ad9910_service.digital_ramp_rates,
            AD9910_DRR_DATA_LENGTH);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_DRR;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_DRR:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_APPLY_SWEEP;
        }
        break;

    case AD9910_SERVICE_STATE_APPLY_SWEEP:
        if (AD9910_IOUpdate(&g_ad9910_service.device) != HAL_OK) {
            ad9910_service_set_error();
            break;
        }
        ad9910_service_set_sweep_hold(0U);
        g_ad9910_service.sweep_active = 1U;
        g_ad9910_service.state = AD9910_SERVICE_STATE_READY;
        break;

    case AD9910_SERVICE_STATE_WRITE_FIXED_CFR2:
        status = ad9910_service_write_register(AD9910_REGISTER_CFR2,
                                                g_ad9910_fixed_cfr2,
                                                AD9910_CFR_DATA_LENGTH);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_FIXED_CFR2;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_FIXED_CFR2:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_PROFILE0;
        }
        break;

    case AD9910_SERVICE_STATE_WRITE_RAM_DISABLE_CFR1:
        status = ad9910_service_write_register(AD9910_REGISTER_CFR1,
                                                g_ad9910_cfr1,
                                                AD9910_CFR_DATA_LENGTH);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_RAM_DISABLE_CFR1;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_RAM_DISABLE_CFR1:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_APPLY_RAM_DISABLE;
        }
        break;

    case AD9910_SERVICE_STATE_APPLY_RAM_DISABLE:
        if (AD9910_IOUpdate(&g_ad9910_service.device) != HAL_OK) {
            ad9910_service_set_error();
            break;
        }
        g_ad9910_service.ram_active = 0U;
        g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_RAM_BASE_FTW;
        break;

    case AD9910_SERVICE_STATE_WRITE_RAM_BASE_FTW:
        status = ad9910_service_write_register(AD9910_REGISTER_FTW,
                                                g_ad9910_service.base_ftw,
                                                AD9910_FTW_DATA_LENGTH);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_RAM_BASE_FTW;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_RAM_BASE_FTW:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_RAM_BASE_POW;
        }
        break;

    case AD9910_SERVICE_STATE_WRITE_RAM_BASE_POW:
        status = ad9910_service_write_register(AD9910_REGISTER_POW,
                                                g_ad9910_service.base_pow,
                                                AD9910_POW_DATA_LENGTH);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_RAM_BASE_POW;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_RAM_BASE_POW:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_RAM_BASE_ASF;
        }
        break;

    case AD9910_SERVICE_STATE_WRITE_RAM_BASE_ASF:
        status = ad9910_service_write_register(AD9910_REGISTER_ASF,
                                                g_ad9910_service.base_asf,
                                                AD9910_ASF_DATA_LENGTH);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_RAM_BASE_ASF;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_RAM_BASE_ASF:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_RAM_PROFILE;
        }
        break;

    case AD9910_SERVICE_STATE_WRITE_RAM_PROFILE:
        status = ad9910_service_write_register(
            (uint8_t)(AD9910_REGISTER_PROFILE0 +
                      g_ad9910_service.profile_write_index),
            g_ad9910_service.ram_profile,
            AD9910_PROFILE_DATA_LENGTH);
        if (status == HAL_OK) {
            ad9910_service_select_profile_gpio(
                g_ad9910_service.profile_write_index);
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_RAM_PROFILE;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_RAM_PROFILE:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_APPLY_RAM_PROFILE;
        }
        break;

    case AD9910_SERVICE_STATE_APPLY_RAM_PROFILE:
        if (AD9910_IOUpdate(&g_ad9910_service.device) != HAL_OK) {
            ad9910_service_set_error();
            break;
        }
        g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_RAM_DATA;
        break;

    case AD9910_SERVICE_STATE_WRITE_RAM_DATA:
        status = ad9910_service_write_register(AD9910_REGISTER_RAM,
                                                g_ad9910_service.ram_data,
                                                g_ad9910_service.ram_data_length);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_RAM_DATA;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_RAM_DATA:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_RAM_PLAYBACK_CFR1;
        }
        break;

    case AD9910_SERVICE_STATE_WRITE_RAM_PLAYBACK_CFR1:
        status = ad9910_service_write_register(AD9910_REGISTER_CFR1,
                                                g_ad9910_service.ram_cfr1,
                                                AD9910_CFR_DATA_LENGTH);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_RAM_PLAYBACK_CFR1;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_RAM_PLAYBACK_CFR1:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_APPLY_RAM_PLAYBACK;
        }
        break;

    case AD9910_SERVICE_STATE_APPLY_RAM_PLAYBACK:
        if (AD9910_IOUpdate(&g_ad9910_service.device) != HAL_OK) {
            ad9910_service_set_error();
            break;
        }
        g_ad9910_service.sweep_active = 0U;
        g_ad9910_service.ram_active = 1U;
        g_ad9910_service.state = AD9910_SERVICE_STATE_READY;
        break;

    case AD9910_SERVICE_STATE_WRITE_RAM_STOP_CFR1:
        status = ad9910_service_write_register(AD9910_REGISTER_CFR1,
                                                g_ad9910_cfr1,
                                                AD9910_CFR_DATA_LENGTH);
        if (status == HAL_OK) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_WAIT_RAM_STOP_CFR1;
        }
        break;

    case AD9910_SERVICE_STATE_WAIT_RAM_STOP_CFR1:
        if (event == AD9910_TRANSFER_EVENT_COMPLETE) {
            g_ad9910_service.state = AD9910_SERVICE_STATE_APPLY_RAM_STOP;
        }
        break;

    case AD9910_SERVICE_STATE_APPLY_RAM_STOP:
        if (AD9910_IOUpdate(&g_ad9910_service.device) != HAL_OK) {
            ad9910_service_set_error();
            break;
        }
        g_ad9910_service.ram_active = 0U;
        g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_FIXED_CFR2;
        break;

    case AD9910_SERVICE_STATE_READY:
        if (g_ad9910_service.ram_stop_requested != 0U) {
            g_ad9910_service.ram_stop_requested = 0U;
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_RAM_STOP_CFR1;
        } else if (g_ad9910_service.ram_start_requested != 0U) {
            g_ad9910_service.ram_start_requested = 0U;
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_RAM_DISABLE_CFR1;
        } else if (g_ad9910_service.sweep_stop_requested != 0U) {
            g_ad9910_service.sweep_stop_requested = 0U;
            g_ad9910_service.sweep_stop_in_progress = 1U;
            ad9910_service_set_sweep_hold(1U);
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_FIXED_CFR2;
        } else if (g_ad9910_service.sweep_start_requested != 0U) {
            g_ad9910_service.sweep_start_requested = 0U;
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_SWEEP_CFR2;
        } else if (g_ad9910_service.profile_update_requested != 0U) {
            g_ad9910_service.profile_update_requested = 0U;
            g_ad9910_service.state = AD9910_SERVICE_STATE_WRITE_PROFILE0;
        }
        break;

    case AD9910_SERVICE_STATE_ERROR:
    default:
        break;
    }
}

HAL_StatusTypeDef AD9910_Service_SetFrequency(uint32_t frequency_hz)
{
    if (frequency_hz > AD9910_MAX_FREQUENCY_HZ) {
        return HAL_ERROR;
    }

    if ((g_ad9910_service.state != AD9910_SERVICE_STATE_READY) ||
        (g_ad9910_service.sweep_active != 0U) ||
        (g_ad9910_service.sweep_start_requested != 0U) ||
        (g_ad9910_service.ram_active != 0U) ||
        (g_ad9910_service.ram_start_requested != 0U)) {
        return HAL_BUSY;
    }

    g_ad9910_service.frequency_hz = frequency_hz;
    g_ad9910_service.profiles[g_ad9910_service.selected_profile].frequency_hz =
        frequency_hz;
    g_ad9910_service.profile_write_index = g_ad9910_service.selected_profile;
    ad9910_service_build_profile(
        &g_ad9910_service.profiles[g_ad9910_service.selected_profile]);
    g_ad9910_service.profile_update_requested = 1U;

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_Service_SetAmplitude(uint16_t amplitude)
{
    if (amplitude > AD9910_MAX_AMPLITUDE) {
        return HAL_ERROR;
    }

    if ((g_ad9910_service.state != AD9910_SERVICE_STATE_READY) ||
        (g_ad9910_service.ram_active != 0U) ||
        (g_ad9910_service.ram_start_requested != 0U)) {
        return HAL_BUSY;
    }

    g_ad9910_service.amplitude = amplitude;
    g_ad9910_service.profiles[g_ad9910_service.selected_profile].amplitude =
        amplitude;
    g_ad9910_service.profile_write_index = g_ad9910_service.selected_profile;
    ad9910_service_build_profile(
        &g_ad9910_service.profiles[g_ad9910_service.selected_profile]);
    g_ad9910_service.profile_update_requested = 1U;

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_Service_SetPhaseOffset(uint16_t phase_offset)
{
    if ((g_ad9910_service.state != AD9910_SERVICE_STATE_READY) ||
        (g_ad9910_service.ram_active != 0U) ||
        (g_ad9910_service.ram_start_requested != 0U)) {
        return HAL_BUSY;
    }

    g_ad9910_service.phase_offset = phase_offset;
    g_ad9910_service.profiles[g_ad9910_service.selected_profile].phase_offset =
        phase_offset;
    g_ad9910_service.profile_write_index = g_ad9910_service.selected_profile;
    ad9910_service_build_profile(
        &g_ad9910_service.profiles[g_ad9910_service.selected_profile]);
    g_ad9910_service.profile_update_requested = 1U;

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_Service_SetTone(const ad9910_tone_config_t *config)
{
    if (ad9910_service_tone_config_is_valid(config) == 0U) {
        return HAL_ERROR;
    }

    if ((g_ad9910_service.state != AD9910_SERVICE_STATE_READY) ||
        (g_ad9910_service.sweep_active != 0U) ||
        (g_ad9910_service.sweep_start_requested != 0U) ||
        (g_ad9910_service.ram_active != 0U) ||
        (g_ad9910_service.ram_start_requested != 0U)) {
        return HAL_BUSY;
    }

    g_ad9910_service.profiles[g_ad9910_service.selected_profile] = *config;
    g_ad9910_service.profile_write_index = g_ad9910_service.selected_profile;
    ad9910_service_update_selected_profile_status();
    ad9910_service_build_profile(config);
    g_ad9910_service.profile_update_requested = 1U;

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_Service_ProgramProfile(
    uint8_t profile_index,
    const ad9910_tone_config_t *config)
{
    if ((profile_index >= AD9910_PROFILE_COUNT) ||
        (ad9910_service_tone_config_is_valid(config) == 0U)) {
        return HAL_ERROR;
    }

    if ((g_ad9910_service.state != AD9910_SERVICE_STATE_READY) ||
        (g_ad9910_service.sweep_active != 0U) ||
        (g_ad9910_service.profile_update_requested != 0U) ||
        (g_ad9910_service.ram_active != 0U) ||
        (g_ad9910_service.ram_start_requested != 0U)) {
        return HAL_BUSY;
    }

    g_ad9910_service.profiles[profile_index] = *config;
    g_ad9910_service.profile_valid_mask |= (uint8_t)(1U << profile_index);
    g_ad9910_service.profile_write_index = profile_index;
    ad9910_service_build_profile(config);
    g_ad9910_service.profile_update_requested = 1U;

    if (profile_index == g_ad9910_service.selected_profile) {
        ad9910_service_update_selected_profile_status();
    }

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_Service_SelectProfile(uint8_t profile_index)
{
    if ((profile_index >= AD9910_PROFILE_COUNT) ||
        ((g_ad9910_service.profile_valid_mask &
          (uint8_t)(1U << profile_index)) == 0U)) {
        return HAL_ERROR;
    }

    if ((g_ad9910_service.state != AD9910_SERVICE_STATE_READY) ||
        (g_ad9910_service.sweep_active != 0U) ||
        (g_ad9910_service.profile_update_requested != 0U) ||
        (g_ad9910_service.ram_active != 0U) ||
        (g_ad9910_service.ram_start_requested != 0U)) {
        return HAL_BUSY;
    }

    ad9910_service_select_profile_gpio(profile_index);
    g_ad9910_service.selected_profile = profile_index;
    ad9910_service_update_selected_profile_status();

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_Service_StartFrequencySweep(
    const ad9910_frequency_sweep_config_t *config)
{
    if (ad9910_service_sweep_config_is_valid(config) == 0U) {
        return HAL_ERROR;
    }

    if ((g_ad9910_service.state != AD9910_SERVICE_STATE_READY) ||
        (g_ad9910_service.sweep_start_requested != 0U) ||
        (g_ad9910_service.sweep_stop_requested != 0U) ||
        (g_ad9910_service.ram_active != 0U) ||
        (g_ad9910_service.ram_start_requested != 0U)) {
        return HAL_BUSY;
    }

    ad9910_service_build_sweep_registers(config);
    ad9910_service_set_sweep_direction(1U);
    ad9910_service_set_sweep_hold(0U);
    g_ad9910_service.sweep_start_requested = 1U;

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_Service_StopFrequencySweep(uint32_t frequency_hz)
{
    if (frequency_hz > AD9910_MAX_FREQUENCY_HZ) {
        return HAL_ERROR;
    }

    if ((g_ad9910_service.state != AD9910_SERVICE_STATE_READY) ||
        (g_ad9910_service.sweep_start_requested != 0U) ||
        (g_ad9910_service.sweep_stop_requested != 0U)) {
        return HAL_BUSY;
    }

    if (g_ad9910_service.sweep_active == 0U) {
        return AD9910_Service_SetFrequency(frequency_hz);
    }

    g_ad9910_service.frequency_hz = frequency_hz;
    g_ad9910_service.profiles[g_ad9910_service.selected_profile].frequency_hz =
        frequency_hz;
    g_ad9910_service.profile_write_index = g_ad9910_service.selected_profile;
    ad9910_service_build_profile(
        &g_ad9910_service.profiles[g_ad9910_service.selected_profile]);
    g_ad9910_service.profile_update_requested = 0U;
    g_ad9910_service.sweep_stop_requested = 1U;

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_Service_StartRamPlayback(
    const ad9910_ram_playback_config_t *config)
{
    if (ad9910_service_ram_config_is_valid(config) == 0U) {
        return HAL_ERROR;
    }

    if ((g_ad9910_service.state != AD9910_SERVICE_STATE_READY) ||
        (g_ad9910_service.ram_start_requested != 0U) ||
        (g_ad9910_service.ram_stop_requested != 0U) ||
        (g_ad9910_service.sweep_start_requested != 0U) ||
        (g_ad9910_service.sweep_stop_requested != 0U)) {
        return HAL_BUSY;
    }

    ad9910_service_set_sweep_hold(0U);
    ad9910_service_build_ram_registers(config);
    g_ad9910_service.ram_start_requested = 1U;

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_Service_StopRamPlayback(uint32_t frequency_hz)
{
    if (frequency_hz > AD9910_MAX_FREQUENCY_HZ) {
        return HAL_ERROR;
    }

    if ((g_ad9910_service.state != AD9910_SERVICE_STATE_READY) ||
        (g_ad9910_service.ram_start_requested != 0U) ||
        (g_ad9910_service.ram_stop_requested != 0U)) {
        return HAL_BUSY;
    }

    if (g_ad9910_service.ram_active == 0U) {
        return AD9910_Service_SetFrequency(frequency_hz);
    }

    g_ad9910_service.frequency_hz = frequency_hz;
    g_ad9910_service.profiles[g_ad9910_service.selected_profile].frequency_hz =
        frequency_hz;
    g_ad9910_service.profile_write_index = g_ad9910_service.selected_profile;
    ad9910_service_build_profile(
        &g_ad9910_service.profiles[g_ad9910_service.selected_profile]);
    g_ad9910_service.ram_stop_requested = 1U;

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_Service_SetFrequencySweepDirection(uint8_t direction_up)
{
    if ((g_ad9910_service.state != AD9910_SERVICE_STATE_READY) ||
        (g_ad9910_service.sweep_active == 0U)) {
        return HAL_BUSY;
    }

    ad9910_service_set_sweep_direction(direction_up);
    return HAL_OK;
}

HAL_StatusTypeDef AD9910_Service_SetFrequencySweepHold(uint8_t hold)
{
    if ((g_ad9910_service.state != AD9910_SERVICE_STATE_READY) ||
        (g_ad9910_service.sweep_active == 0U)) {
        return HAL_BUSY;
    }

    ad9910_service_set_sweep_hold(hold);
    return HAL_OK;
}

void AD9910_Service_OnRampLimitEvent(void)
{
    /* 中断中只累计端点事件，逻辑决策由主循环或上层状态机完成。 */
    ++g_ad9910_service.ramp_limit_event_count;
}

uint8_t AD9910_Service_IsFrequencySweepActive(void)
{
    return g_ad9910_service.sweep_active;
}

uint8_t AD9910_Service_GetFrequencySweepDirection(void)
{
    return g_ad9910_service.sweep_direction_up;
}

uint8_t AD9910_Service_GetFrequencySweepHold(void)
{
    return g_ad9910_service.sweep_hold;
}

uint32_t AD9910_Service_GetRampLimitEventCount(void)
{
    return g_ad9910_service.ramp_limit_event_count;
}

uint8_t AD9910_Service_IsRamPlaybackActive(void)
{
    return g_ad9910_service.ram_active;
}

ad9910_ram_destination_t AD9910_Service_GetRamDestination(void)
{
    return g_ad9910_service.ram_destination;
}

ad9910_ram_mode_t AD9910_Service_GetRamMode(void)
{
    return g_ad9910_service.ram_mode;
}

uint16_t AD9910_Service_GetRamSampleCount(void)
{
    return g_ad9910_service.ram_sample_count;
}

uint16_t AD9910_Service_GetRamAddressStepRate(void)
{
    return g_ad9910_service.ram_address_step_rate;
}

ad9910_service_state_t AD9910_Service_GetState(void)
{
    return g_ad9910_service.state;
}

uint32_t AD9910_Service_GetFrequency(void)
{
    return g_ad9910_service.frequency_hz;
}

uint16_t AD9910_Service_GetAmplitude(void)
{
    return g_ad9910_service.amplitude;
}

uint16_t AD9910_Service_GetPhaseOffset(void)
{
    return g_ad9910_service.phase_offset;
}

uint8_t AD9910_Service_GetSelectedProfile(void)
{
    return g_ad9910_service.selected_profile;
}

uint32_t AD9910_Service_GetLastHalError(void)
{
    return g_ad9910_service.last_hal_error;
}
