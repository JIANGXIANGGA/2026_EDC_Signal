#ifndef AD9910_H
#define AD9910_H

#include <stdint.h>

#include "stm32g4xx_hal.h"

#define AD9910_REGISTER_CFR1                0x00U
#define AD9910_REGISTER_CFR2                0x01U
#define AD9910_REGISTER_CFR3                0x02U
#define AD9910_REGISTER_FTW                  0x07U
#define AD9910_REGISTER_POW                  0x08U
#define AD9910_REGISTER_ASF                  0x09U
#define AD9910_REGISTER_DRL                  0x0BU
#define AD9910_REGISTER_DRS                  0x0CU
#define AD9910_REGISTER_DRR                  0x0DU
#define AD9910_REGISTER_PROFILE0            0x0EU
#define AD9910_REGISTER_RAM                  0x16U
#define AD9910_PROFILE_COUNT                 8U
#define AD9910_CFR_DATA_LENGTH              4U
#define AD9910_FTW_DATA_LENGTH              4U
#define AD9910_POW_DATA_LENGTH              2U
#define AD9910_ASF_DATA_LENGTH              4U
#define AD9910_DRL_DATA_LENGTH              8U
#define AD9910_DRS_DATA_LENGTH              8U
#define AD9910_DRR_DATA_LENGTH              4U
#define AD9910_PROFILE_DATA_LENGTH          8U
#define AD9910_RAM_WORD_SIZE                 4U
#define AD9910_RAM_WORD_COUNT                1024U
#define AD9910_RAM_DATA_LENGTH               (AD9910_RAM_WORD_COUNT * AD9910_RAM_WORD_SIZE)
#define AD9910_MAX_REGISTER_DATA_LENGTH     AD9910_RAM_DATA_LENGTH
#define AD9910_DMA_FRAME_LENGTH             (1U + AD9910_MAX_REGISTER_DATA_LENGTH)
#define AD9910_SYSTEM_CLOCK_HZ              1000000000ULL
#define AD9910_MAX_FREQUENCY_HZ             420000000U
#define AD9910_MAX_AMPLITUDE                0x3FFFU
#define AD9910_MAX_PHASE_DEGREES             359U
#define AD9910_MAX_PHASE_OFFSET              0xFFFFU

typedef struct {
    GPIO_TypeDef *csb_port;
    uint16_t csb_pin;
    GPIO_TypeDef *io_update_port;
    uint16_t io_update_pin;
} ad9910_pin_config_t;

typedef enum {
    AD9910_TRANSFER_EVENT_NONE = 0,
    AD9910_TRANSFER_EVENT_COMPLETE,
    AD9910_TRANSFER_EVENT_ERROR
} ad9910_transfer_event_t;

typedef enum {
    AD9910_RAM_DESTINATION_FREQUENCY = 0,
    AD9910_RAM_DESTINATION_PHASE,
    AD9910_RAM_DESTINATION_AMPLITUDE,
    AD9910_RAM_DESTINATION_POLAR
} ad9910_ram_destination_t;

typedef enum {
    AD9910_RAM_MODE_DIRECT_SWITCH = 0,
    AD9910_RAM_MODE_RAMP_UP = 1,
    AD9910_RAM_MODE_BIDIRECTIONAL_RAMP = 2,
    AD9910_RAM_MODE_CONTINUOUS_BIDIRECTIONAL_RAMP = 3,
    AD9910_RAM_MODE_CONTINUOUS_RECIRCULATE = 4
} ad9910_ram_mode_t;

typedef struct {
    SPI_HandleTypeDef *spi;
    ad9910_pin_config_t pins;
    uint8_t tx_frame[AD9910_DMA_FRAME_LENGTH];
    volatile uint8_t transfer_active;
    volatile uint8_t transfer_complete;
    volatile uint8_t transfer_error;
    uint32_t last_hal_error;
} ad9910_t;

HAL_StatusTypeDef AD9910_Init(ad9910_t *device, SPI_HandleTypeDef *spi, const ad9910_pin_config_t *pins);

HAL_StatusTypeDef AD9910_WriteRegisterDMA(ad9910_t *device,
                                          uint8_t register_address,
                                          const uint8_t *data,
                                          uint16_t data_length);

ad9910_transfer_event_t AD9910_Process(ad9910_t *device);
HAL_StatusTypeDef AD9910_IOUpdate(ad9910_t *device);
uint32_t AD9910_GetLastHalError(const ad9910_t *device);

uint32_t AD9910_FrequencyToFTW(uint32_t frequency_hz);
uint16_t AD9910_AmplitudePercentToASF(uint8_t amplitude_percent);
uint16_t AD9910_PhaseDegreesToPOW(uint16_t phase_degrees);

void AD9910_BuildProfile0(uint8_t profile[AD9910_PROFILE_DATA_LENGTH], uint16_t amplitude, uint16_t phase_offset, uint32_t frequency_tuning_word);
void AD9910_BuildFTW(uint8_t ftw[AD9910_FTW_DATA_LENGTH],
                     uint32_t frequency_tuning_word);
void AD9910_BuildPOW(uint8_t pow[AD9910_POW_DATA_LENGTH],
                     uint16_t phase_offset);
void AD9910_BuildASF(uint8_t asf[AD9910_ASF_DATA_LENGTH],
                     uint16_t amplitude);
void AD9910_BuildDigitalRampLimits(uint8_t limits[AD9910_DRL_DATA_LENGTH],
                                   uint32_t lower_ftw,
                                   uint32_t upper_ftw);
void AD9910_BuildDigitalRampSteps(uint8_t steps[AD9910_DRS_DATA_LENGTH],
                                  uint32_t positive_step_ftw,
                                  uint32_t negative_step_ftw);
void AD9910_BuildDigitalRampRates(uint8_t rates[AD9910_DRR_DATA_LENGTH],
                                  uint16_t positive_rate,
                                  uint16_t negative_rate);
void AD9910_BuildRamProfile(uint8_t profile[AD9910_PROFILE_DATA_LENGTH],
                            uint16_t start_address,
                            uint16_t end_address,
                            uint16_t address_step_rate,
                            ad9910_ram_mode_t mode,
                            uint8_t no_dwell_high,
                            uint8_t zero_crossing);
void AD9910_BuildCFR1RamConfig(uint8_t cfr1[AD9910_CFR_DATA_LENGTH],
                               const uint8_t base_cfr1[AD9910_CFR_DATA_LENGTH],
                               ad9910_ram_destination_t destination,
                               uint8_t ram_enable);
void AD9910_BuildCFR1RamPlayback(uint8_t cfr1[AD9910_CFR_DATA_LENGTH],
                                 const uint8_t base_cfr1[AD9910_CFR_DATA_LENGTH],
                                 ad9910_ram_destination_t destination);
uint32_t AD9910_BuildRamFrequencyWord(uint32_t frequency_hz);
uint32_t AD9910_BuildRamPhaseWord(uint16_t phase_degrees);
uint32_t AD9910_BuildRamAmplitudeWord(uint16_t amplitude);
uint32_t AD9910_BuildRamPolarWord(uint16_t phase_degrees,
                                  uint16_t amplitude);
void AD9910_PackRamWords(uint8_t *destination,
                         const uint32_t *ram_words,
                         uint16_t word_count);
uint16_t AD9910_RamPlaybackRateToStepRate(uint32_t playback_rate_hz);

#endif
