#include "ad9910.h"

#include <stddef.h>

#define AD9910_IO_UPDATE_PULSE_NOP_COUNT 256U

/* 当前硬件只使用一个 AD9910，因此 DMA 回调由该实例接收。 */
static ad9910_t *g_ad9910_dma_device;

static void ad9910_set_csb(const ad9910_t *device, GPIO_PinState state)
{
    HAL_GPIO_WritePin(device->pins.csb_port, device->pins.csb_pin, state);
}

static void ad9910_set_io_update(const ad9910_t *device, GPIO_PinState state)
{
    HAL_GPIO_WritePin(device->pins.io_update_port,
                      device->pins.io_update_pin,
                      state);
}

static uint8_t ad9910_pins_are_valid(const ad9910_pin_config_t *pins)
{
    return (pins != NULL) &&
           (pins->csb_port != NULL) &&
           (pins->io_update_port != NULL);
}

HAL_StatusTypeDef AD9910_Init(ad9910_t *device,
                              SPI_HandleTypeDef *spi,
                              const ad9910_pin_config_t *pins)
{
    if ((device == NULL) || (spi == NULL) || !ad9910_pins_are_valid(pins)) {
        return HAL_ERROR;
    }

    if ((g_ad9910_dma_device != NULL) && (g_ad9910_dma_device != device)) {
        return HAL_BUSY;
    }

    device->spi = spi;
    device->pins = *pins;
    device->transfer_active = 0U;
    device->transfer_complete = 0U;
    device->transfer_error = 0U;
    device->last_hal_error = HAL_SPI_ERROR_NONE;
    g_ad9910_dma_device = device;

    /* 空闲时 CSB 必须为高，避免上电阶段被误识别为串行写操作。 */
    ad9910_set_csb(device, GPIO_PIN_SET);
    ad9910_set_io_update(device, GPIO_PIN_RESET);

    return HAL_OK;
}

HAL_StatusTypeDef AD9910_WriteRegisterDMA(ad9910_t *device,
                                          uint8_t register_address,
                                          const uint8_t *data,
                                          uint16_t data_length)
{
    HAL_StatusTypeDef status;

    if ((device == NULL) || (device->spi == NULL) || (data == NULL) ||
        (data_length == 0U) ||
        (data_length > AD9910_MAX_REGISTER_DATA_LENGTH) ||
        ((register_address & 0x80U) != 0U)) {
        return HAL_ERROR;
    }

    if (device->transfer_active != 0U) {
        return HAL_BUSY;
    }

    /* 写操作最高位为 0，地址和全部数据必须在同一帧中发送。 */
    device->tx_frame[0] = register_address;
    for (uint16_t index = 0U; index < data_length; ++index) {
        device->tx_frame[index + 1U] = data[index];
    }

    device->transfer_complete = 0U;
    device->transfer_error = 0U;
    device->last_hal_error = HAL_SPI_ERROR_NONE;
    device->transfer_active = 1U;
    ad9910_set_csb(device, GPIO_PIN_RESET);

    status = HAL_SPI_Transmit_DMA(device->spi,
                                  device->tx_frame,
                                  (uint16_t)(data_length + 1U));
    if (status != HAL_OK) {
        ad9910_set_csb(device, GPIO_PIN_SET);
        device->transfer_active = 0U;
        device->last_hal_error = HAL_SPI_GetError(device->spi);
    }

    return status;
}

ad9910_transfer_event_t AD9910_Process(ad9910_t *device)
{
    if ((device == NULL) || (device->transfer_active == 0U)) {
        return AD9910_TRANSFER_EVENT_NONE;
    }

    if (device->transfer_error != 0U) {
        ad9910_set_csb(device, GPIO_PIN_SET);
        device->transfer_active = 0U;
        device->transfer_error = 0U;
        device->transfer_complete = 0U;
        return AD9910_TRANSFER_EVENT_ERROR;
    }

    if (device->transfer_complete != 0U) {
        ad9910_set_csb(device, GPIO_PIN_SET);
        device->transfer_active = 0U;
        device->transfer_complete = 0U;
        return AD9910_TRANSFER_EVENT_COMPLETE;
    }

    return AD9910_TRANSFER_EVENT_NONE;
}

HAL_StatusTypeDef AD9910_IOUpdate(ad9910_t *device)
{
    if ((device == NULL) || (device->transfer_active != 0U)) {
        return HAL_BUSY;
    }

    ad9910_set_io_update(device, GPIO_PIN_SET);
    /* 在 170 MHz HCLK 下保持超过 1 us，满足模块 I/O UPDATE 脉宽要求。 */
    for (uint32_t index = 0U; index < AD9910_IO_UPDATE_PULSE_NOP_COUNT; ++index) {
        __NOP();
    }
    ad9910_set_io_update(device, GPIO_PIN_RESET);

    return HAL_OK;
}

uint32_t AD9910_GetLastHalError(const ad9910_t *device)
{
    return (device != NULL) ? device->last_hal_error : HAL_SPI_ERROR_NONE;
}

uint32_t AD9910_FrequencyToFTW(uint32_t frequency_hz)
{
    uint64_t numerator;

    if (frequency_hz > AD9910_MAX_FREQUENCY_HZ) {
        return 0U;
    }

    numerator = ((uint64_t)frequency_hz << 32) +
                (AD9910_SYSTEM_CLOCK_HZ / 2ULL);
    return (uint32_t)(numerator / AD9910_SYSTEM_CLOCK_HZ);
}

uint16_t AD9910_RamPlaybackRateToStepRate(uint32_t playback_rate_hz)
{
    uint64_t numerator;
    uint64_t step_rate;

    if (playback_rate_hz == 0U) {
        return 0U;
    }

    numerator = AD9910_SYSTEM_CLOCK_HZ + (2ULL * playback_rate_hz);
    step_rate = numerator / (4ULL * playback_rate_hz);
    if (step_rate == 0ULL) {
        step_rate = 1ULL;
    }
    if (step_rate > 0xFFFFULL) {
        step_rate = 0xFFFFULL;
    }

    return (uint16_t)step_rate;
}

uint16_t AD9910_AmplitudePercentToASF(uint8_t amplitude_percent)
{
    if (amplitude_percent > 100U) {
        amplitude_percent = 100U;
    }

    return (uint16_t)((((uint32_t)amplitude_percent * AD9910_MAX_AMPLITUDE) +
                       50U) /
                      100U);
}

uint16_t AD9910_PhaseDegreesToPOW(uint16_t phase_degrees)
{
    uint32_t normalized_degrees;

    normalized_degrees = phase_degrees % 360U;
    return (uint16_t)(((normalized_degrees * 65536UL) + 180UL) / 360UL);
}

void AD9910_BuildProfile0(uint8_t profile[AD9910_PROFILE_DATA_LENGTH],
                          uint16_t amplitude,
                          uint16_t phase_offset,
                          uint32_t frequency_tuning_word)
{
    if (profile == NULL) {
        return;
    }

    if (amplitude > AD9910_MAX_AMPLITUDE) {
        amplitude = AD9910_MAX_AMPLITUDE;
    }

    /* Profile0 数据顺序为 ASF[13:0]、POW[15:0]、FTW[31:0]，均为高字节在前。 */
    profile[0] = (uint8_t)((amplitude >> 8) & 0x3FU);
    profile[1] = (uint8_t)amplitude;

    profile[2] = (uint8_t)(phase_offset >> 8);
    profile[3] = (uint8_t)phase_offset;
    
    profile[4] = (uint8_t)(frequency_tuning_word >> 24);
    profile[5] = (uint8_t)(frequency_tuning_word >> 16);
    profile[6] = (uint8_t)(frequency_tuning_word >> 8);
    profile[7] = (uint8_t)frequency_tuning_word;
}

void AD9910_BuildFTW(uint8_t ftw[AD9910_FTW_DATA_LENGTH],
                     uint32_t frequency_tuning_word)
{
    if (ftw == NULL) {
        return;
    }

    ftw[0] = (uint8_t)(frequency_tuning_word >> 24);
    ftw[1] = (uint8_t)(frequency_tuning_word >> 16);
    ftw[2] = (uint8_t)(frequency_tuning_word >> 8);
    ftw[3] = (uint8_t)frequency_tuning_word;
}

void AD9910_BuildPOW(uint8_t pow[AD9910_POW_DATA_LENGTH],
                     uint16_t phase_offset)
{
    if (pow == NULL) {
        return;
    }

    pow[0] = (uint8_t)(phase_offset >> 8);
    pow[1] = (uint8_t)phase_offset;
}

void AD9910_BuildASF(uint8_t asf[AD9910_ASF_DATA_LENGTH],
                     uint16_t amplitude)
{
    if (asf == NULL) {
        return;
    }

    if (amplitude > AD9910_MAX_AMPLITUDE) {
        amplitude = AD9910_MAX_AMPLITUDE;
    }

    asf[0] = 0U;
    asf[1] = 0U;
    asf[2] = (uint8_t)(amplitude >> 6);
    asf[3] = (uint8_t)((amplitude & 0x3FU) << 2);
}

static void ad9910_write_u32_be(uint8_t data[4], uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

void AD9910_BuildDigitalRampLimits(uint8_t limits[AD9910_DRL_DATA_LENGTH],
                                   uint32_t lower_ftw,
                                   uint32_t upper_ftw)
{
    if (limits == NULL) {
        return;
    }

    /* DRL 的高 32 位为上限，低 32 位为下限。 */
    ad9910_write_u32_be(&limits[0], upper_ftw);
    ad9910_write_u32_be(&limits[4], lower_ftw);
}

void AD9910_BuildDigitalRampSteps(uint8_t steps[AD9910_DRS_DATA_LENGTH],
                                  uint32_t positive_step_ftw,
                                  uint32_t negative_step_ftw)
{
    if (steps == NULL) {
        return;
    }

    /* DRS 的高 32 位为下降步进，低 32 位为上升步进。 */
    ad9910_write_u32_be(&steps[0], negative_step_ftw);
    ad9910_write_u32_be(&steps[4], positive_step_ftw);
}

void AD9910_BuildDigitalRampRates(uint8_t rates[AD9910_DRR_DATA_LENGTH],
                                  uint16_t positive_rate,
                                  uint16_t negative_rate)
{
    if (rates == NULL) {
        return;
    }

    /* DRR 的高 16 位为下降速率，低 16 位为上升速率。 */
    rates[0] = (uint8_t)(negative_rate >> 8);
    rates[1] = (uint8_t)negative_rate;
    rates[2] = (uint8_t)(positive_rate >> 8);
    rates[3] = (uint8_t)positive_rate;
}

void AD9910_BuildRamProfile(uint8_t profile[AD9910_PROFILE_DATA_LENGTH],
                            uint16_t start_address,
                            uint16_t end_address,
                            uint16_t address_step_rate,
                            ad9910_ram_mode_t mode,
                            uint8_t no_dwell_high,
                            uint8_t zero_crossing)
{
    if (profile == NULL) {
        return;
    }

    if (start_address >= AD9910_RAM_WORD_COUNT) {
        start_address = AD9910_RAM_WORD_COUNT - 1U;
    }
    if (end_address >= AD9910_RAM_WORD_COUNT) {
        end_address = AD9910_RAM_WORD_COUNT - 1U;
    }

    profile[0] = 0U;
    profile[1] = (uint8_t)(address_step_rate >> 8);
    profile[2] = (uint8_t)address_step_rate;
    profile[3] = (uint8_t)(end_address >> 2);
    profile[4] = (uint8_t)((end_address & 0x03U) << 6);
    profile[5] = (uint8_t)(start_address >> 2);
    profile[6] = (uint8_t)((start_address & 0x03U) << 6);
    profile[7] = (uint8_t)((no_dwell_high != 0U) ? 0x20U : 0U);
    profile[7] |= (uint8_t)((zero_crossing != 0U) ? 0x08U : 0U);
    profile[7] |= (uint8_t)mode & 0x07U;
}

void AD9910_BuildCFR1RamPlayback(uint8_t cfr1[AD9910_CFR_DATA_LENGTH],
                                 const uint8_t base_cfr1[AD9910_CFR_DATA_LENGTH],
                                 ad9910_ram_destination_t destination)
{
    if ((cfr1 == NULL) || (base_cfr1 == NULL)) {
        return;
    }

    for (uint8_t index = 0U; index < AD9910_CFR_DATA_LENGTH; ++index) {
        cfr1[index] = base_cfr1[index];
    }

    cfr1[0] &= (uint8_t)~0xE0U;
    cfr1[0] |= 0x80U;
    cfr1[0] |= (uint8_t)(((uint8_t)destination & 0x03U) << 5);
}

uint32_t AD9910_BuildRamFrequencyWord(uint32_t frequency_hz)
{
    return AD9910_FrequencyToFTW(frequency_hz);
}

uint32_t AD9910_BuildRamPhaseWord(uint16_t phase_degrees)
{
    return ((uint32_t)AD9910_PhaseDegreesToPOW(phase_degrees)) << 16;
}

uint32_t AD9910_BuildRamAmplitudeWord(uint16_t amplitude)
{
    if (amplitude > AD9910_MAX_AMPLITUDE) {
        amplitude = AD9910_MAX_AMPLITUDE;
    }

    return ((uint32_t)amplitude) << 18;
}

uint32_t AD9910_BuildRamPolarWord(uint16_t phase_degrees,
                                  uint16_t amplitude)
{
    if (amplitude > AD9910_MAX_AMPLITUDE) {
        amplitude = AD9910_MAX_AMPLITUDE;
    }

    return (((uint32_t)AD9910_PhaseDegreesToPOW(phase_degrees)) << 16) |
           (((uint32_t)amplitude) << 2);
}

void AD9910_PackRamWords(uint8_t *destination,
                         const uint32_t *ram_words,
                         uint16_t word_count)
{
    if ((destination == NULL) || (ram_words == NULL)) {
        return;
    }

    for (uint16_t index = 0U; index < word_count; ++index) {
        const uint32_t word = ram_words[index];
        destination[(uint32_t)index * 4U] = (uint8_t)(word >> 24);
        destination[((uint32_t)index * 4U) + 1U] = (uint8_t)(word >> 16);
        destination[((uint32_t)index * 4U) + 2U] = (uint8_t)(word >> 8);
        destination[((uint32_t)index * 4U) + 3U] = (uint8_t)word;
    }
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if ((g_ad9910_dma_device != NULL) &&
        (hspi == g_ad9910_dma_device->spi) &&
        (g_ad9910_dma_device->transfer_active != 0U)) {
        /* 中断只通知 DMA 已完成，GPIO 时序由主循环处理。 */
        g_ad9910_dma_device->transfer_complete = 1U;
    }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    if ((g_ad9910_dma_device != NULL) &&
        (hspi == g_ad9910_dma_device->spi) &&
        (g_ad9910_dma_device->transfer_active != 0U)) {
        g_ad9910_dma_device->last_hal_error = HAL_SPI_GetError(hspi);
        g_ad9910_dma_device->transfer_error = 1U;
    }
}
