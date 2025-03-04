/**
 * @file
 * @brief hardware input output abstraction for Buddy board
 */

#include <inttypes.h>

#include "hwio.h"
#include "config.h"
#include <device/hal.h>
#include "cmsis_os.h"
#include "gpio.h"
#include "adc.hpp"
#include "Arduino.h"
#include "loadcell.hpp"
#include "timer_defaults.h"
#include "hwio_pindef.h"
#include "bsod.h"
#include "main.h"
#include "config_buddy_2209_02.h"
#include "fanctl.hpp"
#include "appmain.hpp"
#include "Marlin.h"
#include "MarlinPin.hpp"
#include <option/has_puppies.h>
#include <option/has_loadcell.h>
#include <option/has_gui.h>
#include <option/has_modularbed.h>

#if ENABLED(PRUSA_TOOLCHANGER)
    #include "../../lib/Marlin/Marlin/src/module/prusa/toolchanger.h"
#endif
#if ENABLED(MODULAR_HEATBED)
    #include "../../lib/Marlin/Marlin/src/module/modular_heatbed.h"
#endif

#if (BOARD_IS_XBUDDY)
    #include "hw_configuration.hpp"
#endif

#if HAS_PUPPIES()
    #include "puppies/Dwarf.hpp"
#endif

// #define HAS_BEEPER_WITHOUT_PWM

namespace {
/**
 * @brief hwio Marlin wrapper errors
 */
enum {
    HWIO_ERR_UNINI_DIG_RD = 0x01, //!< uninitialized digital read
    HWIO_ERR_UNINI_DIG_WR, //!< uninitialized digital write
    HWIO_ERR_UNINI_ANA_RD, //!< uninitialized analog read
    HWIO_ERR_UNINI_ANA_WR, //!< uninitialized analog write
    HWIO_ERR_UNDEF_DIG_RD, //!< undefined pin digital read
    HWIO_ERR_UNDEF_DIG_WR, //!< undefined pin digital write
    HWIO_ERR_UNDEF_ANA_RD, //!< undefined pin analog write
    HWIO_ERR_UNDEF_ANA_WR, //!< undefined pin analog write
};

enum {
    _FAN_ID_MIN = HWIO_PWM_FAN1,
    _FAN_ID_MAX = HWIO_PWM_FAN,
};

// buddy pwm output pins
const uint32_t _pwm_pin32[] = {
    MARLIN_PIN(HEAT0),
    MARLIN_PIN(BED_HEAT),
    MARLIN_PIN(FAN1),
    MARLIN_PIN(FAN)
};

enum {
    _PWM_CNT = (sizeof(_pwm_pin32) / sizeof(uint32_t))
};

} // end anonymous namespace

/**
 * @brief analog output pins
 */
static const uint32_t _dac_pin32[] = {};
// buddy analog output maximum values
static const int _dac_max[] = { 0 };
static const size_t _DAC_CNT = sizeof(_dac_pin32) / sizeof(uint32_t);

static const int _FAN_CNT = _FAN_ID_MAX - _FAN_ID_MIN + 1;

// this value is compared to new value (to avoid rounding errors)
static int _tim1_period_us = GEN_PERIOD_US(TIM1_default_Prescaler, TIM1_default_Period);
static int _tim3_period_us = GEN_PERIOD_US(TIM3_default_Prescaler, TIM3_default_Period);

static const uint32_t _pwm_chan[] = {
    TIM_CHANNEL_3, //_PWM_HEATER_BED
    TIM_CHANNEL_4, //_PWM_HEATER_0
    TIM_CHANNEL_1, //_PWM_FAN1
    TIM_CHANNEL_2, //_PWM_FAN
};

static TIM_HandleTypeDef *_pwm_p_htim[] = {
    &htim3, //_PWM_HEATER_BED
    &htim3, //_PWM_HEATER_0
    &htim1, //_PWM_FAN1
    &htim1, //_PWM_FAN
};

static int *const _pwm_period_us[] = {
    &_tim3_period_us, //_PWM_HEATER_BED
    &_tim3_period_us, //_PWM_HEATER_0
    &_tim1_period_us, //_PWM_FAN1
    &_tim1_period_us, //_PWM_FAN
};

// buddy pwm output maximum values
static constexpr int _pwm_max[] = { TIM3_default_Period, TIM3_default_Period, TIM1_default_Period, TIM1_default_Period };

static const TIM_OC_InitTypeDef sConfigOC_default = {
    TIM_OCMODE_PWM1, // OCMode
    0, // Pulse
    TIM_OCPOLARITY_HIGH, // OCPolarity
    TIM_OCNPOLARITY_HIGH, // OCNPolarity
    TIM_OCFAST_DISABLE, // OCFastMode
    TIM_OCIDLESTATE_RESET, // OCIdleState
    TIM_OCNIDLESTATE_RESET // OCNIdleState
};

// buddy pwm output maximum values  as arduino analogWrite
static constexpr int _pwm_analogWrite_max = 255;
// buddy fan output values  as arduino analogWrite
static int _pwm_analogWrite_val[_PWM_CNT] = { 0, 0, 0, 0 };

static int hwio_jogwheel_enabled = 0;

static float hwio_beeper_vol = 1.0F;
#if HAS_BEEPER_WITHOUT_PWM
static std::atomic<uint32_t> hwio_beeper_pulses = 0;
static uint32_t hwio_beeper_period = 0;
#else
static uint32_t hwio_beeper_del = 0;
#endif

/*****************************************************************************
 * private function declarations
 * */
static void __pwm_set_val(TIM_HandleTypeDef *htim, uint32_t chan, int val);
static void _hwio_pwm_analogWrite_set_val(int i_pwm, int val);
static void _hwio_pwm_set_val(int i_pwm, int val);
static uint32_t _pwm_get_chan(int i_pwm);
static TIM_HandleTypeDef *_pwm_get_htim(int i_pwm);
static constexpr int is_pwm_id_valid(int i_pwm);

static metric_t metric_nozzle_pwm = METRIC("nozzle_pwm", METRIC_VALUE_INTEGER, 1000, METRIC_HANDLER_DISABLE_ALL);
static metric_t metric_bed_pwm = METRIC("bed_pwm", METRIC_VALUE_INTEGER, 1000, METRIC_HANDLER_DISABLE_ALL);

//--------------------------------------
// analog output functions

int hwio_dac_get_cnt(void) // number of analog outputs
{ return _DAC_CNT; }

int hwio_dac_get_max(int i_dac) // analog output maximum value
{ return _dac_max[i_dac]; }

void hwio_dac_set_val([[maybe_unused]] int i_dac, [[maybe_unused]] int val) // write analog output
{
}

//--------------------------------------
// pwm output functions

static constexpr int is_pwm_id_valid(int i_pwm) {
    return ((i_pwm >= 0) && (i_pwm < static_cast<int>(_PWM_CNT)));
}

int hwio_pwm_get_cnt(void) // number of pwm outputs
{ return _PWM_CNT; }

static constexpr int hwio_pwm_get_max(int i_pwm) // pwm output maximum value
{
    if (!is_pwm_id_valid(i_pwm)) {
        return -1;
    }
    return _pwm_max[i_pwm];
}

// affects only prescaler
// affects multiple channels
void hwio_pwm_set_period_us(int i_pwm, int T_us) // set pwm resolution
{
    if (!is_pwm_id_valid(i_pwm)) {
        return;
    }
    int *ptr_period_us = _pwm_period_us[i_pwm];

    if (T_us == *ptr_period_us) {
        return;
    }

    int prescaler = T_us * (int32_t)TIM_BASE_CLK_MHZ / (_pwm_max[i_pwm] + 1) - 1;
    hwio_pwm_set_prescaler(i_pwm, prescaler);
    // update seconds
    *ptr_period_us = T_us;
}

int hwio_pwm_get_period_us(int i_pwm) {
    if (!is_pwm_id_valid(i_pwm)) {
        return -1;
    }
    return *_pwm_period_us[i_pwm];
}

void hwio_pwm_set_prescaler(int i_pwm, int prescaler) {
    if (hwio_pwm_get_prescaler(i_pwm) == prescaler) {
        return;
    }

    TIM_HandleTypeDef *htim = _pwm_get_htim(i_pwm);

    // uint32_t           chan = _pwm_get_chan(i_pwm);
    // uint32_t           cmp  = __HAL_TIM_GET_COMPARE(htim,chan);

    htim->Init.Prescaler = prescaler;
    //__pwm_set_val(htim, chan, cmp);

    __HAL_TIM_SET_PRESCALER(htim, prescaler);

    // calculate micro seconds
    int T_us = GEN_PERIOD_US(prescaler, htim->Init.Period);
    // update micro
    int *ptr_period_us = _pwm_period_us[i_pwm];
    *ptr_period_us = T_us;
}

int hwio_pwm_get_prescaler(int i_pwm) {
    if (!is_pwm_id_valid(i_pwm)) {
        return -1;
    }
    TIM_HandleTypeDef *htim = _pwm_get_htim(i_pwm);
    return htim->Init.Prescaler;
}

// values should be:
// 0000 0000 0000 0000 -exp = 0
// 0000 0000 0000 0001 -exp = 1
// 0000 0000 0000 0011 -exp = 2
// 0000 0000 0000 0111 -exp = 3
//..
// 0111 1111 1111 1111 -exp = 15
// 1111 1111 1111 1111 -exp = 16

void hwio_pwm_set_prescaler_exp2(int i_pwm, int exp) {
    uint32_t prescaler = (1 << (exp)) - 1;
    hwio_pwm_set_prescaler(i_pwm, prescaler);
}

// reading value set by hwio_pwm_set_prescaler_exp2
int hwio_pwm_get_prescaler_log2(int i_pwm) {
    if (!is_pwm_id_valid(i_pwm)) {
        return -1;
    }
    uint32_t prescaler = hwio_pwm_get_prescaler(i_pwm) + 1;
    int index = 0;

    while (prescaler != 0) {
        ++index;
        prescaler = prescaler >> 1;
    }

    return index - 1;
}

uint32_t _pwm_get_chan(int i_pwm) {
    if (!is_pwm_id_valid(i_pwm)) {
        return -1;
    }
    return _pwm_chan[i_pwm];
}

TIM_HandleTypeDef *_pwm_get_htim(int i_pwm) {
    if (!is_pwm_id_valid(i_pwm)) {
        i_pwm = 0;
    }

    return _pwm_p_htim[i_pwm];
}

void hwio_pwm_set_val(int i_pwm, uint32_t val) // write pwm output and update _pwm_analogWrite_val
{
    if (!is_pwm_id_valid(i_pwm)) {
        return;
    }

    uint32_t chan = _pwm_get_chan(i_pwm);
    TIM_HandleTypeDef *htim = _pwm_get_htim(i_pwm);
    uint32_t cmp = __HAL_TIM_GET_COMPARE(htim, chan);

    if ((_pwm_analogWrite_val[i_pwm] ^ val) || (cmp != val)) {
        _hwio_pwm_set_val(i_pwm, val);

        // update _pwm_analogWrite_val
        int pwm_max = hwio_pwm_get_max(i_pwm);

        uint32_t pulse = (val * _pwm_analogWrite_max) / pwm_max;
        _pwm_analogWrite_val[i_pwm] = pulse; // arduino compatible
    }
}

void _hwio_pwm_set_val(int i_pwm, int val) // write pwm output
{
    uint32_t chan = _pwm_get_chan(i_pwm);
    TIM_HandleTypeDef *htim = _pwm_get_htim(i_pwm);
    if ((chan == static_cast<uint32_t>(-1)) || htim->Instance == 0) {
        return;
    }

    __pwm_set_val(htim, chan, val);
}

void __pwm_set_val(TIM_HandleTypeDef *htim, uint32_t pchan, int val) // write pwm output
{
    if (htim->Init.Period) {
        TIM_OC_InitTypeDef sConfigOC = sConfigOC_default;
        if (val) {
            sConfigOC.Pulse = val;
        } else {
            sConfigOC.Pulse = htim->Init.Period;
            if (sConfigOC.OCPolarity == TIM_OCPOLARITY_HIGH) {
                sConfigOC.OCPolarity = TIM_OCPOLARITY_LOW;
            } else {
                sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
            }
        }
        if (HAL_TIM_PWM_Stop(htim, pchan) != HAL_OK) {
            Error_Handler();
        }
        if (HAL_TIM_PWM_ConfigChannel(htim, &sConfigOC, pchan) != HAL_OK) {
            Error_Handler();
        }
        if (HAL_TIM_PWM_Start(htim, pchan) != HAL_OK) {
            Error_Handler();
        }
    } else {
        if (HAL_TIM_PWM_Stop(htim, pchan) != HAL_OK) {
            Error_Handler();
        }
    }
}

void _hwio_pwm_analogWrite_set_val(int i_pwm, int val) {
    if (!is_pwm_id_valid(i_pwm)) {
        return;
    }

    switch (i_pwm) {
    case HWIO_PWM_HEATER_0:
        metric_record_integer(&metric_nozzle_pwm, val);
        break;
    case HWIO_PWM_HEATER_BED:
        metric_record_integer(&metric_bed_pwm, val);
        break;
    }

    if (_pwm_analogWrite_val[i_pwm] != val) {
        const int32_t pwm_max = hwio_pwm_get_max(i_pwm);
        const uint32_t pulse = (val * pwm_max) / _pwm_analogWrite_max;
        hwio_pwm_set_val(i_pwm, pulse);
        _pwm_analogWrite_val[i_pwm] = val;
    }
}

//--------------------------------------
// fan control functions

int hwio_fan_get_cnt(void) // number of fans
{ return _FAN_CNT; }

void hwio_fan_set_pwm(int i_fan, int val) {
    i_fan += _FAN_ID_MIN;
    if ((i_fan >= _FAN_ID_MIN) && (i_fan <= _FAN_ID_MAX)) {
        _hwio_pwm_analogWrite_set_val(i_fan, val);
    }
}

//--------------------------------------
// Jogwheel

void hwio_jogwheel_enable(void) {
    hwio_jogwheel_enabled = 1;
}

void hwio_jogwheel_disable(void) {
    hwio_jogwheel_enabled = 0;
}

//--------------------------------------
// Beeper

float hwio_beeper_get_vol(void) {
    return hwio_beeper_vol;
}

void hwio_beeper_set_vol(float vol) {
    if (vol < 0) {
        vol *= -1;
    }
    if (vol > 1) {
        vol = 1;
    }
    hwio_beeper_vol = vol;
}

#if HAS_GUI() && !HAS_BEEPER_WITHOUT_PWM
void hwio_beeper_set_pwm(uint32_t per, uint32_t pul) {
    TIM_OC_InitTypeDef sConfigOC {};
    if (per) {
        htim2.Init.Prescaler = 0;
        htim2.Init.CounterMode = TIM_COUNTERMODE_DOWN;
        htim2.Init.Period = per;
        htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
        HAL_TIM_Base_Init(&htim2);
        sConfigOC.OCMode = TIM_OCMODE_PWM1;
        if (pul) {
            sConfigOC.Pulse = pul;
            sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
        } else {
            sConfigOC.Pulse = per;
            sConfigOC.OCPolarity = TIM_OCPOLARITY_LOW;
        }
        sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
        HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1);
        HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
    } else {
        HAL_TIM_PWM_Stop(&htim2, TIM_CHANNEL_1);
    }
}
#else
void hwio_beeper_set_pwm([[maybe_unused]] uint32_t per, [[maybe_unused]] uint32_t pul) {} // Without display, there is no beeper to beep.
#endif

void hwio_beeper_tone(float frq, uint32_t del) {
    uint32_t per;
    uint32_t pul;
    if (frq && del && hwio_beeper_vol) {
        if (frq < 0) {
            frq *= -1;
        }
        if (frq > 100000) {
            frq = 100000;
        }
#if HAS_BEEPER_WITHOUT_PWM
        per = (uint32_t)(1'000.0F / frq);
        pul = (uint32_t)(del / per);
        hwio_beeper_pulses = pul;
        hwio_beeper_period = per;
#else
        per = (uint32_t)(84000000.0F / frq);
        pul = (uint32_t)(per * hwio_beeper_vol / 2);
        hwio_beeper_set_pwm(per, pul);
        hwio_beeper_del = del;
#endif
    } else {
        hwio_beeper_notone();
    }
}

void hwio_beeper_tone2(float frq, uint32_t del, float vol) {
    hwio_beeper_set_vol(vol);
    hwio_beeper_tone(frq, del);
}

void hwio_beeper_notone(void) {
#if HAS_BEEPER_WITHOUT_PWM
    hwio_beeper_pulses = 0;
#else
    hwio_beeper_set_pwm(0, 0);
#endif
}

void hwio_update_1ms(void) {
#if !HAS_BEEPER_WITHOUT_PWM
    if ((hwio_beeper_del) && ((--hwio_beeper_del) == 0)) {
        hwio_beeper_set_pwm(0, 0);
    }
#elif HAS_GUI() && !(_DEBUG)
    static uint32_t skips = 0;
    if (skips < hwio_beeper_period - 1) {
        skips++;
        return;
    } else {
        skips = 0;
    }

    if (hwio_beeper_pulses > 0) {
        if (hwio_beeper_pulses % 2) {
            buddy::hw::Buzzer.reset();
        } else {
            buddy::hw::Buzzer.set();
        }
        hwio_beeper_pulses -= 1;
    }
#endif
}

#if (BOARD_IS_XBUDDY && HAS_TEMP_HEATBREAK)
extern "C" uint8_t hwio_get_loveboard_bomid() {
    return buddy::hw::Configuration::Instance().get_love_board().bomID;
}
#endif

//--------------------------------------
// Arduino digital/analog read/write error handler

void hwio_arduino_error(int err, uint32_t pin32) {
    const int text_max_len = 64;
    char text[text_max_len];

    strlcat(text, "HWIO error\n", text_max_len);
    switch (err) {
    case HWIO_ERR_UNINI_DIG_RD:
    case HWIO_ERR_UNINI_DIG_WR:
    case HWIO_ERR_UNINI_ANA_RD:
    case HWIO_ERR_UNINI_ANA_WR:
        strlcat(text, "uninitialized\n", text_max_len);
        break;
    case HWIO_ERR_UNDEF_DIG_RD:
    case HWIO_ERR_UNDEF_DIG_WR:
    case HWIO_ERR_UNDEF_ANA_RD:
    case HWIO_ERR_UNDEF_ANA_WR:
        strlcat(text, "undefined\n", text_max_len);
        break;
    }

    snprintf(text + strlen(text), text_max_len - strlen(text),
        "pin #%i (0x%02x)\n", (int)pin32, (uint8_t)pin32);

    switch (err) {
    case HWIO_ERR_UNINI_DIG_RD:
    case HWIO_ERR_UNINI_DIG_WR:
    case HWIO_ERR_UNDEF_DIG_RD:
    case HWIO_ERR_UNDEF_DIG_WR:
        strlcat(text, "digital ", text_max_len);
        break;
    case HWIO_ERR_UNINI_ANA_RD:
    case HWIO_ERR_UNINI_ANA_WR:
    case HWIO_ERR_UNDEF_ANA_RD:
    case HWIO_ERR_UNDEF_ANA_WR:
        strlcat(text, "analog ", text_max_len);
        break;
    }

    switch (err) {
    case HWIO_ERR_UNINI_DIG_RD:
    case HWIO_ERR_UNDEF_DIG_RD:
    case HWIO_ERR_UNINI_ANA_RD:
    case HWIO_ERR_UNDEF_ANA_RD:
        strlcat(text, "read", text_max_len);
        break;
    case HWIO_ERR_UNINI_DIG_WR:
    case HWIO_ERR_UNDEF_DIG_WR:
    case HWIO_ERR_UNINI_ANA_WR:
    case HWIO_ERR_UNDEF_ANA_WR:
        strlcat(text, "write", text_max_len);
        break;
    }
    bsod(text);
}

/**
 * @brief Read digital pin to be used from Marlin
 *
 * Use MARLIN_PIN macro when handling special cases (virtual pins)
 * @example @code
 * case MARLIN_PIN(Z_MIN):
 * @endcode
 *
 * @todo Bypass electric signal when reading output pin
 */
int digitalRead(uint32_t marlinPin) {
#if _DEBUG
    if (!HAL_GPIO_Initialized) {
        hwio_arduino_error(HWIO_ERR_UNINI_DIG_RD, marlinPin); // error: uninitialized digital read
        return 0;
    }
#endif //_DEBUG
    switch (marlinPin) {
#if HAS_LOADCELL()
    case MARLIN_PIN(Z_MIN):
        if (Z_MIN_ENDSTOP_INVERTING) {
            return !loadcell.GetMinZEndstop() && static_cast<bool>(buddy::hw::zDiag.read());
        } else {
            return loadcell.GetMinZEndstop() || static_cast<bool>(buddy::hw::zDiag.read());
        }
#else
    case MARLIN_PIN(Z_MIN):
        return static_cast<bool>(buddy::hw::zMin.read());
#endif
#if HAS_MODULARBED()
    case MARLIN_PIN(DUMMY): {
        return 0;
    }
#endif
    default:
#if _DEBUG
        if (!buddy::hw::physicalPinExist(marlinPin)) {
            hwio_arduino_error(HWIO_ERR_UNDEF_DIG_RD, marlinPin); // error: undefined pin digital read
            return 0;
        }
#endif //_DEBUG
        return gpio_get(marlinPin);
    }
}

/**
 * @brief Write digital pin to be used from Marlin
 *
 * Use MARLIN_PIN macro when handling special cases (virtual pins)
 * @example @code
 * case MARLIN_PIN(FAN):
 * @endcode
 *
 */
void digitalWrite(uint32_t marlinPin, uint32_t ulVal) {
#if _DEBUG
    if (!HAL_GPIO_Initialized) {
        hwio_arduino_error(HWIO_ERR_UNINI_DIG_WR, marlinPin); // error: uninitialized digital write
        return;
    }
#endif //_DEBUG
    switch (marlinPin) {
    case MARLIN_PIN(BED_HEAT):
#if HAS_MODULARBED()
        return;
#else
        _hwio_pwm_analogWrite_set_val(HWIO_PWM_HEATER_BED, ulVal ? _pwm_analogWrite_max : 0);
        return;
#endif
    case MARLIN_PIN(HEAT0):
        _hwio_pwm_analogWrite_set_val(HWIO_PWM_HEATER_0, ulVal ? _pwm_analogWrite_max : 0);
        return;
    case MARLIN_PIN(FAN1):
#if (PRINTER_IS_PRUSA_MK4 || PRINTER_IS_PRUSA_iX)
        _hwio_pwm_analogWrite_set_val(HWIO_PWM_FAN1, ulVal ? 80 : 0);
#elif PRINTER_IS_PRUSA_MK3_5
        Fans::heat_break(0).setPWM(ulVal ? _pwm_analogWrite_max : 0);
#else
        Fans::heat_break(0).setPWM(ulVal ? 180 : 0);
#endif
        return;
    case MARLIN_PIN(FAN):
        Fans::print(0).setPWM(ulVal ? 255 : 0);
        return;
    default:
#if _DEBUG
        if (!buddy::hw::isOutputPin(marlinPin)) {
            hwio_arduino_error(HWIO_ERR_UNDEF_DIG_WR, marlinPin); // error: undefined pin digital write
            return;
        }
#endif //_DEBUG
        gpio_set(marlinPin, ulVal ? 1 : 0);
    }
}

uint32_t analogRead(uint32_t ulPin) {
    if (HAL_ADC_Initialized) {
        switch (ulPin) {

        case MARLIN_PIN(TEMP_BED):
#if HAS_MODULARBED()
            return 0;
#else
            return AdcGet::bed();
#endif

        case MARLIN_PIN(TEMP_0):
#if (BOARD_IS_BUDDY || BOARD_IS_XBUDDY)
            return AdcGet::nozzle();
#endif

#if (HAS_TEMP_HEATBREAK)
        case MARLIN_PIN(TEMP_HEATBREAK):
    #if (BOARD_IS_BUDDY)
            return AdcGet::pinda();
    #elif (BOARD_IS_XBUDDY)
            return AdcGet::heatbreakTemp();
    #endif
#endif // HAS_TEMP_HEATBREAK

        case MARLIN_PIN(THERM2):
            return AdcGet::boardTemp();
        default:
            hwio_arduino_error(HWIO_ERR_UNDEF_ANA_RD, ulPin); // error: undefined pin analog read
        }
    } else {
        hwio_arduino_error(HWIO_ERR_UNINI_ANA_RD, ulPin); // error: uninitialized analog read
    }
    return 0;
}

void analogWrite(uint32_t ulPin, uint32_t ulValue) {
    if (HAL_PWM_Initialized) {
        switch (ulPin) {
        case MARLIN_PIN(FAN1):
            Fans::heat_break(0).setPWM(ulValue);
            return;
        case MARLIN_PIN(FAN):
            Fans::print(0).setPWM(ulValue);
            return;
        case MARLIN_PIN(BED_HEAT):
#if HAS_MODULARBED()
            return;
#else
            _hwio_pwm_analogWrite_set_val(HWIO_PWM_HEATER_BED, ulValue);
            return;
#endif
        case MARLIN_PIN(HEAT0):
            _hwio_pwm_analogWrite_set_val(HWIO_PWM_HEATER_0, ulValue);
            return;
        default:
            hwio_arduino_error(HWIO_ERR_UNDEF_ANA_WR, ulPin); // error: undefined pin analog write
        }
    } else {
        hwio_arduino_error(HWIO_ERR_UNINI_ANA_WR, ulPin); // error: uninitialized analog write
    }
}

void pinMode([[maybe_unused]] uint32_t ulPin, [[maybe_unused]] uint32_t ulMode) {
    // not supported, all pins are configured with Cube
}

void buddy::hw::hwio_configure_board_revision_changed_pins() {
    // no pins whose configuration changes between HW revisions on this board
}
