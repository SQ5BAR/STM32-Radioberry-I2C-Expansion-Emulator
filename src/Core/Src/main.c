#include "main.h"
#include "adc.h"
#include "dma.h"
#include "tim.h"

extern I2C_HandleTypeDef hi2c1;
extern I2C_HandleTypeDef hi2c2;
extern I2C_HandleTypeDef hi2c3;
extern ADC_HandleTypeDef hadc1;
extern TIM_HandleTypeDef htim1;

/* I2C slaves and bias PWM pins. */

/* MCP23008 registers */
#define MCP_IODIR   0x00
#define MCP_IPOL    0x01
#define MCP_GPINTEN 0x02
#define MCP_DEFVAL  0x03
#define MCP_INTCON  0x04
#define MCP_IOCON   0x05
#define MCP_GPPU    0x06
#define MCP_INTF    0x07
#define MCP_INTCAP  0x08
#define MCP_GPIO    0x09
#define MCP_OLAT    0x0A
#define MCP_REGS    0x0B

static volatile uint8_t mcp_regs[MCP_REGS] = {
    0xFF, /* all inputs */
    0x00, /* IPOL */
    0x00, /* GPINTEN */
    0x00, /* DEFVAL */
    0x00, /* INTCON */
    0x00, /* IOCON */
    0x00, /* GPPU */
    0x00, /* INTF */
    0x00, /* INTCAP */
    0x00, /* GPIO */
    0x00  /* OLAT */
};

static volatile uint8_t mcp_ptr = 0x00;
static volatile uint8_t mcp_rx_byte = 0x00;
static volatile uint8_t mcp_rx_phase = 0; /* 0=reg, 1=data */
static volatile uint8_t mcp_tx_byte = 0x00;

/* MCP23008 GPIO update is done in main loop.
 * GP0..GP7: PA4, PB12, PB13, PB14, PB15, PB3, PB5, PB8. */
static volatile uint8_t mcp_gpio_dirty = 1;
static volatile uint8_t mcp_gpio_shadow_iodir = 0xFF;
static volatile uint8_t mcp_gpio_shadow_olat = 0x00;

/* I2C1 recovery and PC13 blink. */
/* Updated from I2C address IRQ, watched by main loop. */
static volatile uint32_t i2c_last_address_ms = 0;
static volatile uint8_t i2c_seen_since_boot = 0;
/* Only serious errors (not normal AF/NACK) request restart. */
static volatile uint8_t i2c_hard_fault_pending = 0;
static volatile uint32_t i2c_hard_fault_ms = 0;
static volatile uint32_t i2c1_activity_tick = 0;
static volatile uint8_t pc13_blink_request = 0;
static uint32_t pc13_blink_until = 0;
static uint8_t pc13_blink_active = 0;

/* MAX11613 emulator */
static volatile uint8_t max_cfg = 0x00;
static volatile uint8_t max_rx_byte = 0x00;
static volatile uint8_t max_tx_buf[8] = {0x00, 0x10, 0x01, 0x70, 0x00, 0x20, 0x00, 0x18};

/* ADC cache for MAX11613: CH0..CH3 = PA0..PA3.
 * DMA is preferred; polling is fallback. */
static volatile uint16_t adc_dma_raw[4] __attribute__((aligned(4))) = {0, 0, 0, 0};
static volatile uint16_t max_adc_cached[4] = {0x0010, 0x0170, 0x0020, 0x0018};
static uint32_t max_adc_last_copy_ms = 0;
static uint8_t max_adc_mode = 0; /* 0=off, 1=DMA, 2=poll */

/* MCP4662 emulator on I2C3 / 0x2C */
static volatile uint8_t mcp4662_rx_byte = 0x00;     /* legacy */
static volatile uint8_t mcp4662_rx_phase = 0;       /* state */
static volatile uint8_t mcp4662_rx_buf[2] = {0x00, 0x00};
static volatile uint8_t mcp4662_cmd = 0x00;
static volatile uint16_t mcp4662_wiper0 = 0x0080;
static volatile uint16_t mcp4662_wiper1 = 0x0080;
static volatile uint8_t mcp4662_tx_buf[2] = {0x80, 0x80};

/* Bias PWM update is done in main loop.
 * 0x00 -> PA9/TIM1_CH2, 0x10 -> PA10/TIM1_CH3. */
static volatile uint8_t bias_pwm_dirty = 1;
static volatile uint8_t bias_pwm0_req = 0x80;
static volatile uint8_t bias_pwm1_req = 0x80;
static uint8_t bias_pwm0_shadow = 0xFF;
static uint8_t bias_pwm1_shadow = 0xFF;

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void mcp_gpio_service(void);
static void i2c1_recovery_service(void);
static void pc13_led_service(void);
static void pc13_blink_ok(void);
static void max_adc_start(void);
static void max_adc_service(void);
static void radio_watchdog_init(void);
static void radio_watchdog_feed(void);
static void bias_pwm_start(void);
static void bias_pwm_service(void);

static uint8_t mcp_read_reg(uint8_t reg)
{
    reg %= MCP_REGS;

    if (reg == MCP_GPIO)
    {
        /* Outputs from OLAT, inputs from GPIO. */
        uint8_t iodir = mcp_regs[MCP_IODIR];
        uint8_t pins = (mcp_regs[MCP_GPIO] & iodir) |
                       (mcp_regs[MCP_OLAT] & (uint8_t)~iodir);
        return pins ^ mcp_regs[MCP_IPOL];
    }

    return mcp_regs[reg];
}

static void mcp_write_reg(uint8_t reg, uint8_t val)
{
    reg %= MCP_REGS;

    switch (reg)
    {
        case MCP_INTF:
        case MCP_INTCAP:
            /* read-only */
            break;

        case MCP_GPIO:
            mcp_regs[MCP_OLAT] = val;
            mcp_regs[MCP_GPIO] = val;
            mcp_gpio_dirty = 1;
            break;

        case MCP_OLAT:
            mcp_regs[MCP_OLAT] = val;
            mcp_regs[MCP_GPIO] = val;
            mcp_gpio_dirty = 1;
            break;

        case MCP_IODIR:
            mcp_regs[MCP_IODIR] = val;
            mcp_gpio_dirty = 1;
            break;

        default:
            mcp_regs[reg] = val;
            break;
    }
}

static uint16_t max_get_cached_channel(uint8_t ch)
{
    uint16_t v;

    if (ch > 3)
        ch = 0;

    __disable_irq();
    v = max_adc_cached[ch];
    __enable_irq();

    return (uint16_t)(v & 0x0FFF);
}

static void max_prepare_tx(void)
{
    /* Return cached CH0..CH3 as 8 bytes. */
    for (uint8_t i = 0; i < 4; i++)
    {
        uint16_t v = max_get_cached_channel(i);
        max_tx_buf[(i * 2) + 0] = (uint8_t)((v >> 8) & 0x0F);
        max_tx_buf[(i * 2) + 1] = (uint8_t)(v & 0xFF);
    }
}

static void start_listen(void)
{
    HAL_I2C_EnableListen_IT(&hi2c1);
    HAL_I2C_EnableListen_IT(&hi2c2);
    HAL_I2C_EnableListen_IT(&hi2c3);
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_ADC1_Init();
    max_adc_start();

    MX_TIM1_Init();
    bias_pwm_start();

    MX_I2C1_Init();
    MX_I2C2_Init();
    MX_I2C3_Init();

    start_listen();

    /* Independent LSI watchdog: also works if the main loop stops running. */
    radio_watchdog_init();

    while (1)
    {
        /* Slow work outside I2C callbacks. */
        mcp_gpio_service();
        max_adc_service();
        bias_pwm_service();
        i2c1_recovery_service();
        pc13_led_service();
        radio_watchdog_feed();
    }
}


static void mcp4662_store_2byte_write(void)
{
    /* One write = control byte + data byte. */
    uint8_t control = mcp4662_rx_buf[0];
    uint8_t data    = mcp4662_rx_buf[1];

    mcp4662_cmd = control;

    switch (control & 0xF0)
    {
        case 0x00:
            mcp4662_wiper0 = data;
            bias_pwm0_req = data;
            bias_pwm_dirty = 1;
            break;

        case 0x10:
            mcp4662_wiper1 = data;
            bias_pwm1_req = data;
            bias_pwm_dirty = 1;
            break;

        default:
            /* Unknown command: keep bus alive. */
            mcp4662_wiper0 = data;
            bias_pwm0_req = data;
            bias_pwm_dirty = 1;
            break;
    }

    mcp4662_rx_phase = 0;
}

static void mcp4662_prepare_read2(void)
{
    /* Plain read returns two wiper bytes. */
    mcp4662_tx_buf[0] = (uint8_t)(mcp4662_wiper0 & 0xFF);
    mcp4662_tx_buf[1] = (uint8_t)(mcp4662_wiper1 & 0xFF);
}

static void mcp4662_reset_state(void)
{
    mcp4662_rx_phase = 0;
    mcp4662_rx_byte = 0x00;
    mcp4662_rx_buf[0] = 0x00;
    mcp4662_rx_buf[1] = 0x00;
}

void HAL_I2C_AddrCallback(I2C_HandleTypeDef *hi2c,
                          uint8_t TransferDirection,
                          uint16_t AddrMatchCode)
{
    (void)AddrMatchCode;

    /* One atomic 32-bit tick write. Never call HAL re-init from this IRQ. */
    i2c_last_address_ms = HAL_GetTick();
    i2c_seen_since_boot = 1U;

    /* LED_ADDR_MATCH_20261020: event == slave address match on any I2C */
    /* IRQ-safe: only set a flag; main loop performs the GPIO operation. */
    pc13_blink_ok();

    if (hi2c->Instance == I2C1)
    {
        i2c1_activity_tick = HAL_GetTick();

        if (TransferDirection == I2C_DIRECTION_TRANSMIT)
        {
            /* First byte is register pointer. Next byte closes the write. */
            mcp_rx_phase = 0;
            HAL_I2C_Slave_Seq_Receive_IT(hi2c,
                                         (uint8_t *)&mcp_rx_byte,
                                         1,
                                         I2C_FIRST_FRAME);
        }
        else
        {
            /* Register read after pointer write. */
            mcp_rx_phase = 0;

            mcp_tx_byte = mcp_read_reg(mcp_ptr);
            HAL_I2C_Slave_Seq_Transmit_IT(hi2c,
                                          (uint8_t *)&mcp_tx_byte,
                                          1,
                                          I2C_LAST_FRAME);
            mcp_ptr = (uint8_t)((mcp_ptr + 1) % MCP_REGS);
        }
    }
    else if (hi2c->Instance == I2C2)
    {
        if (TransferDirection == I2C_DIRECTION_TRANSMIT)
        {
            HAL_I2C_Slave_Seq_Receive_IT(hi2c,
                                         (uint8_t *)&max_rx_byte,
                                         1,
                                         I2C_NEXT_FRAME);
        }
        else
        {
            max_prepare_tx();
            HAL_I2C_Slave_Seq_Transmit_IT(hi2c,
                                          (uint8_t *)max_tx_buf,
                                          8,
                                          I2C_LAST_FRAME);
        }
    }
    else if (hi2c->Instance == I2C3)
    {
        if (TransferDirection == I2C_DIRECTION_TRANSMIT)
        {
            mcp4662_reset_state();
            HAL_I2C_Slave_Seq_Receive_IT(hi2c,
                                         (uint8_t *)mcp4662_rx_buf,
                                         2,
                                         I2C_LAST_FRAME);
        }
        else
        {
            mcp4662_prepare_read2();
            HAL_I2C_Slave_Seq_Transmit_IT(hi2c,
                                          (uint8_t *)mcp4662_tx_buf,
                                          2,
                                          I2C_LAST_FRAME);
        }
    }
}

void HAL_I2C_SlaveRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1)
    {
        if (mcp_rx_phase == 0)
        {
            mcp_ptr = (uint8_t)(mcp_rx_byte % MCP_REGS);
            mcp_rx_phase = 1;

            /* MCP23008 register + value: data is the FINAL byte.
             * Register-pointer-only writes followed by RESTART remain legal. */
            HAL_I2C_Slave_Seq_Receive_IT(hi2c,
                                         (uint8_t *)&mcp_rx_byte,
                                         1,
                                         I2C_LAST_FRAME);
        }
        else
        {
            mcp_write_reg(mcp_ptr, mcp_rx_byte);
            mcp_ptr = (uint8_t)((mcp_ptr + 1) % MCP_REGS);
            i2c1_activity_tick = HAL_GetTick();
            /* MCP23008_TWO_BYTE_STOP_FIX_20261021 */
            /* Do NOT queue a third byte. Let HAL process STOP normally,
             * transition to READY, and re-enable listen in ListenCplt. */
        }
    }
    else if (hi2c->Instance == I2C2)
    {
        max_cfg = max_rx_byte;
        HAL_I2C_Slave_Seq_Receive_IT(hi2c,
                                     (uint8_t *)&max_rx_byte,
                                     1,
                                     I2C_NEXT_FRAME);
    }
    else if (hi2c->Instance == I2C3)
    {
        mcp4662_store_2byte_write();
        HAL_I2C_EnableListen_IT(hi2c);
    }
}

void HAL_I2C_SlaveTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1)
    {
        mcp_rx_phase = 0;
        i2c1_activity_tick = HAL_GetTick();
    }

    HAL_I2C_EnableListen_IT(hi2c);
}

void HAL_I2C_ListenCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1)
    {
        mcp_rx_phase = 0;
        i2c1_activity_tick = HAL_GetTick();
    }
    else if (hi2c->Instance == I2C3)
        mcp4662_reset_state();

    HAL_I2C_EnableListen_IT(hi2c);
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    /*
     * HAL_I2C_ERROR_AF on STOP after an open-ended slave receive is
     * normal for this emulator: e.g. MCP23008 writes register + value.
     * The HAL STOP/Listen completion path will restore slave listen.
     *
     * Crucially, do not DeInit/Init the hardware inside HAL's own ISR.
     * Doing so previously corrupted HAL state and could hold SCL low,
     * freezing the Raspberry Pi master during a band/filter change.
     */
    const uint32_t err = HAL_I2C_GetError(hi2c);

    if (hi2c->Instance == I2C1)
    {
        mcp_rx_phase = 0;
        i2c1_activity_tick = HAL_GetTick();
    }
    else if (hi2c->Instance == I2C3)
    {
        mcp4662_reset_state();
    }

    /* AF (master NACK / short frame termination) needs NO recovery. */
    if ((err & ~(uint32_t)HAL_I2C_ERROR_AF) != 0U)
    {
        /* Deferring reset lets HAL finish the current interrupt first. */
        i2c_hard_fault_ms = HAL_GetTick();
        i2c_hard_fault_pending = 1U;
    }
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLM = 16;
    RCC_OscInitStruct.PLL.PLLN = 336;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
    RCC_OscInitStruct.PLL.PLLQ = 7;

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
        Error_Handler();

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK |
                                  RCC_CLOCKTYPE_SYSCLK |
                                  RCC_CLOCKTYPE_PCLK1 |
                                  RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
        Error_Handler();
}




static uint32_t bias_pwm_value_to_pulse(uint8_t value)
{
    uint32_t arr = __HAL_TIM_GET_AUTORELOAD(&htim1);

    /* 8-bit value to timer compare. */
    return ((uint32_t)value * arr + 127U) / 255U;
}

static void bias_pwm_apply(uint8_t value0, uint8_t value1)
{
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, bias_pwm_value_to_pulse(value0));
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_3, bias_pwm_value_to_pulse(value1));
}

static void bias_pwm_start(void)
{
    /* Start PWM from buffered values. */
    bias_pwm_apply(bias_pwm0_req, bias_pwm1_req);

    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2); /* PA9  */
    HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_3); /* PA10 */

    bias_pwm_dirty = 1;
}

static void bias_pwm_service(void)
{
    uint8_t v0;
    uint8_t v1;

    if (!bias_pwm_dirty)
        return;

    /* Copy request atomically. */
    __disable_irq();
    v0 = bias_pwm0_req;
    v1 = bias_pwm1_req;

    if (v0 == bias_pwm0_shadow && v1 == bias_pwm1_shadow)
    {
        bias_pwm_dirty = 0;
        __enable_irq();
        return;
    }

    bias_pwm0_shadow = v0;
    bias_pwm1_shadow = v1;
    bias_pwm_dirty = 0;
    __enable_irq();

    bias_pwm_apply(v0, v1);
}

static void max_adc_copy_to_cache(const uint16_t v[4])
{
    __disable_irq();
    max_adc_cached[0] = (uint16_t)(v[0] & 0x0FFF);
    max_adc_cached[1] = (uint16_t)(v[1] & 0x0FFF);
    max_adc_cached[2] = (uint16_t)(v[2] & 0x0FFF);
    max_adc_cached[3] = (uint16_t)(v[3] & 0x0FFF);
    __enable_irq();
}

static void max_adc_switch_to_polling(void)
{
    /* Emergency fallback only if HAL_ADC_Start_DMA actually failed.
     * Real zero readings must never force a switch to polling. */
    HAL_ADC_Stop_DMA(&hadc1);
    HAL_ADC_Stop(&hadc1);
    hadc1.Init.ScanConvMode = DISABLE;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.NbrOfConversion = 1;
    hadc1.Init.DMAContinuousRequests = DISABLE;
    hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    if (HAL_ADC_Init(&hadc1) == HAL_OK)
    {
        max_adc_mode = 2;
    }
    else
    {
        i2c_hard_fault_ms = HAL_GetTick();
        i2c_hard_fault_pending = 1U;
    }
}

static void max_adc_start(void)
{
    max_adc_mode = 0;

    /* Start ADC DMA, fallback to polling if needed. */
    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_dma_raw, 4) == HAL_OK)
    {
        max_adc_mode = 1;
    }
    else
    {
        max_adc_switch_to_polling();
    }
}

static void max_adc_poll_once(void)
{
    /* Single conversion per channel; the ADC scan DMA mode remains the
     * default and is not touched during normal operation. */
    static const uint32_t channel[4] = {
        ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3
    };
    uint16_t sample[4] = {0, 0, 0, 0};
    ADC_ChannelConfTypeDef cfg = {0};
    cfg.Rank = 1;
    cfg.SamplingTime = ADC_SAMPLETIME_480CYCLES;

    for (uint8_t i = 0; i < 4; i++)
    {
        cfg.Channel = channel[i];
        if (HAL_ADC_ConfigChannel(&hadc1, &cfg) != HAL_OK)
            return;
        if (HAL_ADC_Start(&hadc1) != HAL_OK)
            return;
        HAL_StatusTypeDef ok = HAL_ADC_PollForConversion(&hadc1, 2);
        if (ok != HAL_OK)
        {
            HAL_ADC_Stop(&hadc1);
            return;
        }
        sample[i] = (uint16_t)(HAL_ADC_GetValue(&hadc1) & 0x0FFF);
        HAL_ADC_Stop(&hadc1);
    }
    max_adc_copy_to_cache(sample);
}

static void max_adc_service(void)
{
    uint32_t now = HAL_GetTick();

    /* Main-loop only. */
    if ((uint32_t)(now - max_adc_last_copy_ms) < 10U)
        return;

    max_adc_last_copy_ms = now;

    if (max_adc_mode == 1)
    {
        uint16_t sample[4];

        sample[0] = (uint16_t)(adc_dma_raw[0] & 0x0FFF);
        sample[1] = (uint16_t)(adc_dma_raw[1] & 0x0FFF);
        sample[2] = (uint16_t)(adc_dma_raw[2] & 0x0FFF);
        sample[3] = (uint16_t)(adc_dma_raw[3] & 0x0FFF);

        max_adc_copy_to_cache(sample);

    }
    else
    {
        max_adc_poll_once();
    }
}


static void pc13_blink_ok(void)
{
    pc13_blink_request = 1;
}

static void pc13_led_service(void)
{
    uint32_t now = HAL_GetTick();

    if (pc13_blink_request)
    {
        pc13_blink_request = 0;
        pc13_blink_active = 1;
        pc13_blink_until = now + 30U;
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
    }

    if (pc13_blink_active && ((int32_t)(now - pc13_blink_until) >= 0))
    {
        pc13_blink_active = 0;
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
    }
}

/* IWDG remains independent of both the HAL tick and I2C IRQ delivery.
 * LSI nominal 32 kHz / 256 / (999+1) -> about 8 seconds.
 * IWDG LSI clock tolerance means this is intentionally approximate. */
static void radio_watchdog_init(void)
{
    IWDG->KR = 0x5555U;  /* unlock PR/RLR */
    IWDG->PR = 6U;       /* divide by 256 */
    IWDG->RLR = 999U;
    IWDG->KR = 0xAAAAU;  /* reload */
    IWDG->KR = 0xCCCCU;  /* start */
}

static void radio_watchdog_feed(void)
{
    IWDG->KR = 0xAAAAU;
}

/* No peripheral DeInit/Init inside interrupts, and no false 100 ms
 * 'stuck' timeout between legitimate master transactions.
 * Any serious peripheral fault -> reset outside ISR after 250 ms.
 * Once I2C activity has been detected, 15 seconds of complete silence
 * -> reset STM32 to release SCL/SDA if a slave was stuck stretching.
 * On reset the activity flag clears, so an idle Pi does NOT reset-loop.
 */
static void i2c1_recovery_service(void)
{
    uint32_t now = HAL_GetTick();

    if (i2c_hard_fault_pending &&
        (uint32_t)(now - i2c_hard_fault_ms) >= 250U)
    {
        NVIC_SystemReset();
    }

    /* A failed filter transaction can strand I2C1 BUSY while ADC/I2C2
     * still talks normally, so global inactivity alone is insufficient.
     * This is only a LAST RESORT full-chip reset, not re-init mid-frame. */
    if (i2c_seen_since_boot)
    {
        HAL_I2C_StateTypeDef st = HAL_I2C_GetState(&hi2c1);
        if (st != HAL_I2C_STATE_READY && st != HAL_I2C_STATE_LISTEN &&
            (uint32_t)(now - i2c1_activity_tick) >= 2000U)
        {
            NVIC_SystemReset();
        }
    }

    if (i2c_seen_since_boot &&
        (uint32_t)(now - i2c_last_address_ms) >= 15000U)
    {
        NVIC_SystemReset();
    }
}


static void mcp_gpio_config_pin(GPIO_TypeDef *port, uint16_t pin, uint8_t output)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    GPIO_InitStruct.Pin = pin;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

    if (output)
    {
        GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    }
    else
    {
        GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    }

    HAL_GPIO_Init(port, &GPIO_InitStruct);
}

static void mcp_gpio_apply_bit(uint8_t bit, GPIO_TypeDef *port, uint16_t pin,
                               uint8_t iodir, uint8_t olat)
{
    uint8_t mask = (uint8_t)(1U << bit);

    if (iodir & mask)
    {
        /* Input = Hi-Z. */
        mcp_gpio_config_pin(port, pin, 0);
    }
    else
    {
        /* Output follows OLAT. */
        mcp_gpio_config_pin(port, pin, 1);
        HAL_GPIO_WritePin(port, pin, (olat & mask) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
}

static void mcp_gpio_apply(uint8_t iodir, uint8_t olat)
{
    mcp_gpio_apply_bit(0, GPIOA, GPIO_PIN_4,  iodir, olat);
    mcp_gpio_apply_bit(1, GPIOB, GPIO_PIN_12, iodir, olat);
    mcp_gpio_apply_bit(2, GPIOB, GPIO_PIN_13, iodir, olat);
    mcp_gpio_apply_bit(3, GPIOB, GPIO_PIN_14, iodir, olat);
    mcp_gpio_apply_bit(4, GPIOB, GPIO_PIN_15, iodir, olat);
    mcp_gpio_apply_bit(5, GPIOB, GPIO_PIN_3,  iodir, olat);
    mcp_gpio_apply_bit(6, GPIOB, GPIO_PIN_5,  iodir, olat);
    mcp_gpio_apply_bit(7, GPIOB, GPIO_PIN_8,  iodir, olat);
}

static void mcp_gpio_service(void)
{
    uint8_t iodir;
    uint8_t olat;

    if (!mcp_gpio_dirty)
        return;

    /* Copy registers atomically. */
    __disable_irq();
    iodir = mcp_regs[MCP_IODIR];
    olat = mcp_regs[MCP_OLAT];

    if (iodir == mcp_gpio_shadow_iodir && olat == mcp_gpio_shadow_olat)
    {
        mcp_gpio_dirty = 0;
        __enable_irq();
        return;
    }

    mcp_gpio_shadow_iodir = iodir;
    mcp_gpio_shadow_olat = olat;
    mcp_gpio_dirty = 0;
    __enable_irq();

    mcp_gpio_apply(iodir, olat);
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);
    GPIO_InitStruct.Pin = GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /* MCP23008 reset: all inputs. */
    mcp_gpio_apply(0xFF, 0x00);
}


void Error_Handler(void)
{
    __disable_irq();
    while (1)
    {
    }
}
