/*
   This file is part of the OpenMV project.
   Copyright (c) 2013/2014 Ibrahim Abdelkader <i.abdalkader@gmail.com>
   This work is licensed under the MIT license, see the file LICENSE for details.

   OV3660 driver.

*/
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sccb.h"
#include "ov5642.h"
#include "ov5642_regs.h"
#include "ov5642_settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(ARDUINO_ARCH_ESP32) && defined(CONFIG_ARDUHAL_ESP_LOG)
#include "esp32-hal-log.h"
#else
#include "esp_log.h"
static const char *TAG = "ov5642";
#endif

//#define REG_DEBUG_ON

static int read_reg(uint8_t slv_addr, const uint16_t reg) {
  int ret = SCCB_Read16(slv_addr, reg);
#ifdef REG_DEBUG_ON
  if (ret < 0) {
    ESP_LOGE(TAG, "READ REG 0x%04x FAILED: %d", reg, ret);
  }
#endif
  return ret;
}

static int check_reg_mask(uint8_t slv_addr, uint16_t reg, uint8_t mask) {
  return (read_reg(slv_addr, reg) & mask) == mask;
}

static int read_reg16(uint8_t slv_addr, const uint16_t reg) {
  int ret = 0, ret2 = 0;
  ret = read_reg(slv_addr, reg);
  if (ret >= 0) {
    ret = (ret & 0xFF) << 8;
    ret2 = read_reg(slv_addr, reg + 1);
    if (ret2 < 0) {
      ret = ret2;
    } else {
      ret |= ret2 & 0xFF;
    }
  }
  return ret;
}


static int write_reg(uint8_t slv_addr, const uint16_t reg, uint8_t value) {
  int ret = 0;
#ifndef REG_DEBUG_ON
  ret = SCCB_Write16(slv_addr, reg, value);
#else
  int old_value = read_reg(slv_addr, reg);
  if (old_value < 0) {
    return old_value;
  }
  if ((uint8_t)old_value != value) {
    ESP_LOGI(TAG, "NEW REG 0x%04x: 0x%02x to 0x%02x", reg, (uint8_t)old_value, value);
    ret = SCCB_Write16(slv_addr, reg, value);
  } else {
    ESP_LOGD(TAG, "OLD REG 0x%04x: 0x%02x", reg, (uint8_t)old_value);
    ret = SCCB_Write16(slv_addr, reg, value);//maybe not?
  }
  if (ret < 0) {
    ESP_LOGE(TAG, "WRITE REG 0x%04x FAILED: %d", reg, ret);
  }
#endif
  return ret;
}

static int set_reg_bits(uint8_t slv_addr, uint16_t reg, uint8_t offset, uint8_t mask, uint8_t value) {
  int ret = 0;
  uint8_t c_value, new_value;
  ret = read_reg(slv_addr, reg);
  if (ret < 0) {
    return ret;
  }
  c_value = ret;
  new_value = (c_value & ~(mask << offset)) | ((value & mask) << offset);
  ret = write_reg(slv_addr, reg, new_value);
  return ret;
}

static int write_regs(uint8_t slv_addr, const uint16_t (*regs)[2])
{
  int i = 0, ret = 0;
  while (!ret && regs[i][0] != REGLIST_TAIL) {
    if (regs[i][0] == REG_DLY) {
      vTaskDelay(regs[i][1] / portTICK_PERIOD_MS);
    } else {
      ret = write_reg(slv_addr, regs[i][0], regs[i][1]);
    }
    i++;
  }
  return ret;
}

static int write_reg16(uint8_t slv_addr, const uint16_t reg, uint16_t value)
{
  if (write_reg(slv_addr, reg, value >> 8) || write_reg(slv_addr, reg + 1, value)) {
    return -1;
  }
  return 0;
}

static int write_addr_reg(uint8_t slv_addr, const uint16_t reg, uint16_t x_value, uint16_t y_value)
{
  if (write_reg16(slv_addr, reg, x_value) || write_reg16(slv_addr, reg + 2, y_value)) {
    return -1;
  }
  return 0;
}

#define write_reg_bits(slv_addr, reg, mask, enable) set_reg_bits(slv_addr, reg, 0, mask, enable?mask:0)

int ov5642_calc_sysclk(int xclk, bool pll_bypass, int pll_multiplier, int pll_sys_div, int pll_pre_div, bool pll_root_2x, int pll_seld5, bool pclk_manual, int pclk_div)
{
  const int pll_pre_div2x_map[] = { 2, 3, 4, 5, 6, 8, 12, 16 };//values are multiplied by two to avoid floats
  const int pll_seld52x_map[] = { 2, 2, 4, 5 };

  if (!pll_sys_div) {
    pll_sys_div = 1;
  }

  int pll_pre_div2x = pll_pre_div2x_map[pll_pre_div];
  int pll_root_div = pll_root_2x ? 2 : 1;
  int pll_seld52x = pll_seld52x_map[pll_seld5];

  int VCO = (xclk / 1000) * pll_multiplier * pll_root_div * 2 / pll_pre_div2x;
  int PLLCLK = pll_bypass ? (xclk) : (VCO * 1000 * 2 / pll_sys_div / pll_seld52x);
  int PCLK = PLLCLK / 2 / ((pclk_manual && pclk_div) ? pclk_div : 1);
  int SYSCLK = PLLCLK / 4;

  ESP_LOGD(TAG, "Calculated VCO: %d KHz, PLLCLK: %d KHz, SYSCLK: %d KHz, PCLK: %d KHz", VCO, PLLCLK / 1000, SYSCLK / 1000, PCLK / 1000);
  return SYSCLK;
}

static int set_pll(sensor_t *sensor, bool bypass, uint8_t multiplier, uint8_t sys_div, uint8_t pre_div, bool root_2x, uint8_t seld5, bool pclk_manual, uint8_t pclk_div) {
  int ret = 0;
  if (multiplier > 31 || sys_div > 15 || pre_div > 7 || pclk_div > 31 || seld5 > 3) {
    ESP_LOGE(TAG, "Invalid arguments");
    return -1;
  }

  // ret = set_pll(sensor, bypass = false, multiplier = 12, sys_div = 1, pre_div = 3, false, 0, true, 2);
  ov5642_calc_sysclk(sensor->xclk_freq_hz, bypass, multiplier, sys_div, pre_div, root_2x, seld5, pclk_manual, pclk_div);
  ret = write_reg(sensor->slv_addr, SC_PLLS_CTRL0, (root_2x ? 0x04 : 0x00) | seld5); // 0x00
  if (ret == 0) {
    ret = write_reg(sensor->slv_addr, SC_PLLS_CTRL1, (sys_div & 0x0f) << 4); //
  }
  if (ret == 0) {
    ret = write_reg(sensor->slv_addr, SC_PLLS_CTRL2, (bypass ? 0x80 : 0x00) | (pclk_div & 0x3f)); // OK
  }
  if (ret == 0) {
    ret = write_reg(sensor->slv_addr, SC_PLLS_CTRL3, (pre_div & 0x7)); // OK
  }
  if (ret == 0) {
    ret = write_reg(sensor->slv_addr, PCLK_RATIO, multiplier  & 0x1F);
  }
  if (ret == 0) {
    ret = write_reg(sensor->slv_addr, VFIFO_CTRL0C, pclk_manual ? 0x22 : 0x20);
  }
  if (ret) {
    ESP_LOGE(TAG, "set_sensor_pll FAILED!");
  }
  return ret;
}

static int set_ae_level(sensor_t *sensor, int level);

static void check_clock(sensor_t *sensor) {
  uint8_t i;
  uint8_t pll_ctrl[4];
  for (i = 0; i < 4; i++) {
    pll_ctrl[i] = read_reg(sensor->slv_addr, 0x300F + i);
  }
  uint8_t PLL_SELD5_MAP[4] = {1, 1, 4, 5};
  double PLL_PRE_DIV2X_MAP[8] = {2, 3, 4, 5, 6, 8, 12, 16};
  bool PLL_BYPASS;
  uint8_t PLL_DIVL, PLL_SELD5_RAW, PLL_DIVS, PLL_DIVM, PLL_DIVP, PLL_PRE_DIV2X_RAW;
  uint8_t PLL_SELD5, PLL_PRE_DIV2X;

  bool FROM_PRE_DIV = (read_reg(sensor->slv_addr, 0x3103) & 0x02) >> 1 ? true : false;

  PLL_PRE_DIV2X_RAW = pll_ctrl[3] & 0x07;
  PLL_SELD5_RAW = pll_ctrl[0] & 0x03;

  PLL_DIVL = (pll_ctrl[0] & 0x04) >> 2;                 // ----
  PLL_SELD5 = PLL_SELD5_MAP[PLL_SELD5_RAW];             // Used
  PLL_DIVS = (pll_ctrl[1] & 0xF0) >> 4;                 // Used
  PLL_DIVM = (pll_ctrl[1] & 0x0F);                      // ---- <- for MIPI
  PLL_BYPASS = pll_ctrl[2] & 0x80 ? true : false;       // Used
  PLL_DIVP = pll_ctrl[2] & 0x3F;                        // Used
  PLL_PRE_DIV2X = PLL_PRE_DIV2X_MAP[PLL_PRE_DIV2X_RAW]; // Used

  ESP_LOGD(TAG, "PLL DIVL[%hhu] SELD5[%hhu] DIVS[%hhu] DIVM[%hhu] BYPASS[%hhu] DIVP[%hhu] PRE_DIV2X[%hhu] FROM_PRE_DIV[%hhu]", PLL_DIVL, PLL_SELD5, PLL_DIVS, PLL_DIVM, PLL_BYPASS?1:0, PLL_DIVP, PLL_PRE_DIV2X, FROM_PRE_DIV?1:0);

  uint32_t XCLK = sensor->xclk_freq_hz;
  uint32_t PLLCLK = PLL_BYPASS ? XCLK : (FROM_PRE_DIV ? XCLK * PLL_PRE_DIV2X / 2 : XCLK);
  uint32_t VCO = PLLCLK * PLL_DIVP * PLL_SELD5;
  uint32_t SYSCLK = PLLCLK * PLL_DIVP / (PLL_DIVS?PLL_DIVS:1) / 4;

  ESP_LOGD(TAG, "XCLK[%uMHz] PLLCLK[%uMHz] VCO[%uMHz] SYSCLK[%uMHz]", XCLK/1000000, PLLCLK/1000000, VCO/1000000, SYSCLK/1000000);

  // int VCO = (xclk / 1000) * pll_multiplier * pll_root_div * 2 / pll_pre_div2x;
  // int PLLCLK = pll_bypass ? (xclk) : (VCO * 1000 * 2 / pll_sys_div / pll_seld52x);
  // int PCLK = PLLCLK / 2 / ((pclk_manual && pclk_div) ? pclk_div : 1);
  // int SYSCLK = PLLCLK / 4;

}

// OV5642 COMAPTIBLE
static int reset(sensor_t *sensor)
{
  int ret = 0;
  // Software Reset: clear all registers and reset them to their default values
  ret = write_reg(sensor->slv_addr, SYSTEM_CTROL0, 0x82);
  if (ret) {
    ESP_LOGE(TAG, "Software Reset FAILED!");
    return ret;
  }
  vTaskDelay(100 / portTICK_PERIOD_MS);
  ret = write_regs(sensor->slv_addr, ov5642_sensor_default_regs);
  if (ret == 0) {
    ESP_LOGD(TAG, "Camera defaults loaded");
    ret = set_ae_level(sensor, 0);
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
  ret = write_regs(sensor->slv_addr, ov5642_auto_focus_regs);
  ret != write_reg(sensor->slv_addr, 0x3f00, 0x03);
  if (ret == 0) {
    write_reg(sensor->slv_addr, 0x3025, 0x01);
    write_reg(sensor->slv_addr, 0x3024, 0x10);
    ESP_LOGD(TAG, "Auto Focus Initiated");
  }

  // check_clock(reset);

  return ret;
}

static int set_pixformat(sensor_t *sensor, pixformat_t pixformat) {
  int ret = 0;
  const uint16_t (*regs)[2];

  switch (pixformat) {
    case PIXFORMAT_YUV422:
      regs = ov5642_sensor_fmt_yuv422;
      break;

    case PIXFORMAT_GRAYSCALE:
      regs = ov5642_sensor_fmt_grayscale;
      break;

    case PIXFORMAT_RGB565:
    case PIXFORMAT_RGB888:
      regs = ov5642_sensor_fmt_rgb565;
      break;

    case PIXFORMAT_JPEG:
      regs = ov5642_sensor_fmt_jpeg;
      break;

    case PIXFORMAT_RAW:
      regs = ov5642_sensor_fmt_raw;
      break;

    default:
      ESP_LOGE(TAG, "Unsupported pixformat: %u", pixformat);
      return -1;
  }

  ret = write_regs(sensor->slv_addr, regs);
  if (ret == 0) {
    sensor->pixformat = pixformat;
    ESP_LOGD(TAG, "Set pixformat to: %u", pixformat);
  }
  return ret;
}

// OV5642 compatible
static int set_image_options(sensor_t *sensor) {
  int ret = 0;
  uint8_t reg18 = 0;
  uint8_t x_bin = 0;
  uint8_t y_bin = 0;

  // enable compression
  if (sensor->pixformat == PIXFORMAT_JPEG) {
    reg18 |= 0x80;
  }

  // binning for small framsizes
  if (sensor->status.framesize <= FRAMESIZE_SVGA) {
    x_bin |= 0x40;
    y_bin |= 0x80;
  }

  // V-Flip
  if (sensor->status.vflip) {
    reg18 |= 0x80;
  }

  // H-Mirror
  if (sensor->status.hmirror) {
    reg18 |= 0x40;
  }

  if (write_reg(sensor->slv_addr, TIMING_TC_REG18, reg18)
      || write_reg(sensor->slv_addr, ANALOG_CONTROL_D, y_bin)
      || write_reg(sensor->slv_addr, ARRAY_CONTROL01, x_bin)) {
    ESP_LOGE(TAG, "Setting Image Options Failed");
    ret = -1;
  }

  ESP_LOGD(TAG, "Set Image Options: Compression: %u, Binning: %u, V-Flip: %u, H-Mirror: %u",
           sensor->pixformat == PIXFORMAT_JPEG, sensor->status.framesize <= FRAMESIZE_SVGA, sensor->status.vflip, sensor->status.hmirror);
  return ret;
}

static int set_framesize(sensor_t *sensor, framesize_t framesize)
{
  int ret = 0;
  framesize_t old_framesize = sensor->status.framesize;
  sensor->status.framesize = framesize;

  if (framesize >= FRAMESIZE_INVALID) {
    ESP_LOGE(TAG, "Invalid framesize: %u", framesize);
    return -1;
  }
  uint16_t w = resolution[framesize][0];
  uint16_t h = resolution[framesize][1];

  //  if (framesize > FRAMESIZE_SVGA) {
  //    ret  = write_reg(sensor->slv_addr, 0x4520, 0xb0)
  //           || write_reg(sensor->slv_addr, X_INCREMENT, 0x11)//odd:1, even: 1
  //           || write_reg(sensor->slv_addr, Y_INCREMENT, 0x11);//odd:1, even: 1
  //  } else {
  //    ret  = write_reg(sensor->slv_addr, 0x4520, 0x0b)
  //           || write_reg(sensor->slv_addr, X_INCREMENT, 0x31)//odd:3, even: 1
  //           || write_reg(sensor->slv_addr, Y_INCREMENT, 0x31);//odd:3, even: 1
  //  }
  //
  //  if (ret) {
  //    goto fail;
  //  }

  ret  = write_addr_reg(sensor->slv_addr, X_ADDR_ST_H, 432, 10)
         || write_addr_reg(sensor->slv_addr, X_ADDR_END_H, 2592, 1944) // 0x3804, 0x3805, 0x3806, 0x3807 to 2079 and 1547
         || write_addr_reg(sensor->slv_addr, X_OUTPUT_SIZE_H, w, h); // 0x3808, 0x3809, 0x380A, 0x380B to w and h

  if (ret) {
    goto fail;
  }

  if (framesize > FRAMESIZE_SVGA) {
    ret  = write_addr_reg(sensor->slv_addr, X_TOTAL_SIZE_H, 3200, 2000)
           || write_reg(sensor->slv_addr, XY_OFFSET, ((12 & 0x0F) << 4) | (2 & 0x0F));
  } else {
    if (framesize == FRAMESIZE_SVGA) {
      ret = write_addr_reg(sensor->slv_addr, X_TOTAL_SIZE_H, 3200, 1000);
    } else {
      ret = write_addr_reg(sensor->slv_addr, X_TOTAL_SIZE_H, 1600, 500);
    }
    if (ret == 0) {
      ret = write_reg(sensor->slv_addr, XY_OFFSET, ((12 & 0x0F) << 4) | (2 & 0x0F));
    }
  }

  if (ret == 0) {
    if (framesize == FRAMESIZE_QSXGA) {
      write_reg(sensor->slv_addr, ISP_CONTROL_01, 0x4f);
    } else {
      write_reg(sensor->slv_addr, ISP_CONTROL_01, 0x7f);
    }
  }

  if (ret == 0) {
    ret = set_image_options(sensor);
  }

  if (ret) {
    goto fail;
  }

  // set_pll(sensor_t *sensor, bool bypass, uint8_t multiplier, uint8_t sys_div, uint8_t pre_div, bool root_2x, uint8_t seld5, bool pclk_manual, uint8_t pclk_div)
  if (sensor->pixformat == PIXFORMAT_JPEG) {
    if (framesize == FRAMESIZE_QSXGA) {
      // TODO !!! PLLの調整 をするところです
      //      write_reg(sensor->slv_addr, 0x3103, 0x03);
      ret = set_pll(sensor, false, 12, 1, 3, false, 0, true, 2);
      //      write_reg(sensor->slv_addr, 0x3011, 0x00);
      //      write_reg(sensor->slv_addr, 0x3012, 0x00);
      //      write_reg(sensor->slv_addr, 0x3010, 0x10);
      //      write_reg(sensor->slv_addr, 0x460C, 0x22);
    } else if (framesize == FRAMESIZE_QXGA) {
      //40MHz SYSCLK and 10MHz PCLK
      ret = set_pll(sensor, false, 24, 1, 3, false, 0, true, 8);
    } else {
      //50MHz SYSCLK and 10MHz PCLK
      ret = set_pll(sensor, false, 30, 1, 3, false, 0, true, 10);
    }
  } else {
    if (framesize > FRAMESIZE_CIF) {
      //10MHz SYSCLK and 10MHz PCLK (6.19 FPS)
      ret = set_pll(sensor, false, 2, 1, 0, false, 0, true, 2);
    } else {
      //25MHz SYSCLK and 10MHz PCLK (15.45 FPS)
      ret = set_pll(sensor, false, 5, 1, 0, false, 0, true, 5);
    }
  }

  if (ret == 0) {
    ESP_LOGD(TAG, "Set framesize to: %ux%u", w, h);
  }
  return ret;

fail:
  sensor->status.framesize = old_framesize;
  ESP_LOGE(TAG, "Setting framesize to: %ux%u failed", w, h);
  return ret;
}

static int set_hmirror(sensor_t *sensor, int enable)
{
  int ret = 0;
  sensor->status.hmirror = enable;
  ret = set_image_options(sensor);
  if (ret == 0) {
    ESP_LOGD(TAG, "Set h-mirror to: %d", enable);
  }
  return ret;
}

static int set_vflip(sensor_t *sensor, int enable)
{
  int ret = 0;
  sensor->status.vflip = enable;
  ret = set_image_options(sensor);
  if (ret == 0) {
    ESP_LOGD(TAG, "Set v-flip to: %d", enable);
  }
  return ret;
}

// OV5642 COMPATIBLE
static int set_quality(sensor_t *sensor, int qs)
{
  int ret = 0;
  ret = write_reg(sensor->slv_addr, COMPRESSION_CTRL07, qs & 0x3f);
  if (ret == 0) {
    sensor->status.quality = qs;
    ESP_LOGD(TAG, "Set quality to: %d", qs);
  }
  return ret;
}

static int set_colorbar(sensor_t *sensor, int enable)
{
  int ret = 0;
  ret = write_reg_bits(sensor->slv_addr, PRE_ISP_TEST_SETTING_1, TEST_COLOR_BAR, enable);
  if (ret == 0) {
    sensor->status.colorbar = enable;
    ESP_LOGD(TAG, "Set colorbar to: %d", enable);
  }
  return ret;
}

static int set_gain_ctrl(sensor_t *sensor, int enable)
{
  int ret = 0;
  ret = write_reg_bits(sensor->slv_addr, AEC_PK_MANUAL, AEC_PK_MANUAL_AGC_MANUALEN, !enable);
  if (ret == 0) {
    ESP_LOGD(TAG, "Set gain_ctrl to: %d", enable);
    sensor->status.agc = enable;
  }
  return ret;
}

static int set_exposure_ctrl(sensor_t *sensor, int enable)
{
  int ret = 0;
  ret = write_reg_bits(sensor->slv_addr, AEC_PK_MANUAL, AEC_PK_MANUAL_AEC_MANUALEN, !enable);
  if (ret == 0) {
    ESP_LOGD(TAG, "Set exposure_ctrl to: %d", enable);
    sensor->status.aec = enable;
  }
  return ret;
}

static int set_whitebal(sensor_t *sensor, int enable)
{
  int ret = 0;
  ret = write_reg_bits(sensor->slv_addr, ISP_CONTROL_01, 0x01, enable);
  if (ret == 0) {
    ESP_LOGD(TAG, "Set awb to: %d", enable);
    sensor->status.awb = enable;
  }
  return ret;
}

//Advanced AWB
static int set_dcw_dsp(sensor_t *sensor, int enable)
{
  int ret = 0;
  ret = write_reg_bits(sensor->slv_addr, 0x5183, 0x80, !enable);
  if (ret == 0) {
    ESP_LOGD(TAG, "Set dcw to: %d", enable);
    sensor->status.dcw = enable;
  }
  return ret;
}

//night mode enable
static int set_aec2(sensor_t *sensor, int enable)
{
  int ret = 0;
  ret = write_reg_bits(sensor->slv_addr, 0x3a00, 0x04, enable);
  if (ret == 0) {
    ESP_LOGD(TAG, "Set aec2 to: %d", enable);
    sensor->status.aec2 = enable;
  }
  return ret;
}

static int set_bpc_dsp(sensor_t *sensor, int enable)
{
  int ret = 0;
  ret = write_reg_bits(sensor->slv_addr, 0x5000, 0x04, enable);
  if (ret == 0) {
    ESP_LOGD(TAG, "Set bpc to: %d", enable);
    sensor->status.bpc = enable;
  }
  return ret;
}

static int set_wpc_dsp(sensor_t *sensor, int enable)
{
  int ret = 0;
  ret = write_reg_bits(sensor->slv_addr, 0x5000, 0x02, enable);
  if (ret == 0) {
    ESP_LOGD(TAG, "Set wpc to: %d", enable);
    sensor->status.wpc = enable;
  }
  return ret;
}

//Gamma enable
static int set_raw_gma_dsp(sensor_t *sensor, int enable)
{
  int ret = 0;
  ret = write_reg_bits(sensor->slv_addr, 0x5000, 0x20, enable);
  if (ret == 0) {
    ESP_LOGD(TAG, "Set raw_gma to: %d", enable);
    sensor->status.raw_gma = enable;
  }
  return ret;
}

static int set_lenc_dsp(sensor_t *sensor, int enable)
{
  int ret = 0;
  ret = write_reg_bits(sensor->slv_addr, 0x5000, 0x80, enable);
  if (ret == 0) {
    ESP_LOGD(TAG, "Set lenc to: %d", enable);
    sensor->status.lenc = enable;
  }
  return ret;
}

static int get_agc_gain(sensor_t *sensor)
{
  int ra = read_reg(sensor->slv_addr, 0x350a);
  if (ra < 0) {
    return 0;
  }
  int rb = read_reg(sensor->slv_addr, 0x350b);
  if (rb < 0) {
    return 0;
  }
  int res = (rb & 0xF0) >> 4 | (ra & 0x03) << 4;
  if (rb & 0x0F) {
    res += 1;
  }
  return res;
}

//real gain
static int set_agc_gain(sensor_t *sensor, int gain)
{
  int ret = 0;
  if (gain < 0) {
    gain = 0;
  } else if (gain > 64) {
    gain = 64;
  }

  //gain value is 6.4 bits float
  //in order to use the max range, we deduct 1/16
  int gainv = gain << 4;
  if (gainv) {
    gainv -= 1;
  }

  ret = write_reg(sensor->slv_addr, 0x350a, gainv >> 8) || write_reg(sensor->slv_addr, 0x350b, gainv & 0xff);
  if (ret == 0) {
    ESP_LOGD(TAG, "Set agc_gain to: %d", gain);
    sensor->status.agc_gain = gain;
  }
  return ret;
}

static int get_aec_value(sensor_t *sensor)
{
  int ra = read_reg(sensor->slv_addr, 0x3500);
  if (ra < 0) {
    return 0;
  }
  int rb = read_reg(sensor->slv_addr, 0x3501);
  if (rb < 0) {
    return 0;
  }
  int rc = read_reg(sensor->slv_addr, 0x3502);
  if (rc < 0) {
    return 0;
  }
  int res = (ra & 0x0F) << 12 | (rb & 0xFF) << 4 | (rc & 0xF0) >> 4;
  return res;
}

static int set_aec_value(sensor_t *sensor, int value)
{
  int ret = 0, max_val = 0;
  max_val = read_reg16(sensor->slv_addr, 0x380e);
  if (max_val < 0) {
    ESP_LOGE(TAG, "Could not read max aec_value");
    return -1;
  }
  if (value > max_val) {
    value = max_val;
  }

  ret =  write_reg(sensor->slv_addr, 0x3500, (value >> 12) & 0x0F)
         || write_reg(sensor->slv_addr, 0x3501, (value >> 4) & 0xFF)
         || write_reg(sensor->slv_addr, 0x3502, (value << 4) & 0xF0);

  if (ret == 0) {
    ESP_LOGD(TAG, "Set aec_value to: %d / %d", value, max_val);
    sensor->status.aec_value = value;
  }
  return ret;
}

// OV5642 COMPATIBLE
static int set_ae_level(sensor_t *sensor, int level)
{
  int ret = 0;
  if (level < -5 || level > 5) {
    return -1;
  }
  //good targets are between 5 and 115
  int target_level = ((level + 5) * 10) + 5;

  int level_high, level_low;
  int fast_high, fast_low;

  level_low = target_level * 23 / 25; //0.92 (0.46)
  level_high = target_level * 27 / 25; //1.08 (2.08)

  fast_low = level_low >> 1;
  fast_high = level_high << 1;

  if (fast_high > 255) {
    fast_high = 255;
  }

  ret =  write_reg(sensor->slv_addr, 0x3a0f, level_high)
         || write_reg(sensor->slv_addr, 0x3a10, level_low)
         || write_reg(sensor->slv_addr, 0x3a1b, level_high)
         || write_reg(sensor->slv_addr, 0x3a1e, level_low)
         || write_reg(sensor->slv_addr, 0x3a11, fast_high)
         || write_reg(sensor->slv_addr, 0x3a1f, fast_low);

  if (ret == 0) {
    ESP_LOGD(TAG, "Set ae_level to: %d", level);
    sensor->status.ae_level = level;
  }
  return ret;
}

static int set_wb_mode(sensor_t *sensor, int mode)
{
  int ret = 0;
  if (mode < 0 || mode > 4) {
    return -1;
  }

  ret = write_reg(sensor->slv_addr, 0x3406, (mode != 0));
  if (ret) {
    return ret;
  }
  switch (mode) {
    case 1://Sunny
      ret  = write_reg16(sensor->slv_addr, 0x3400, 0x5e0) //AWB R GAIN
             || write_reg16(sensor->slv_addr, 0x3402, 0x410) //AWB G GAIN
             || write_reg16(sensor->slv_addr, 0x3404, 0x540);//AWB B GAIN
      break;
    case 2://Cloudy
      ret  = write_reg16(sensor->slv_addr, 0x3400, 0x650) //AWB R GAIN
             || write_reg16(sensor->slv_addr, 0x3402, 0x410) //AWB G GAIN
             || write_reg16(sensor->slv_addr, 0x3404, 0x4f0);//AWB B GAIN
      break;
    case 3://Office
      ret  = write_reg16(sensor->slv_addr, 0x3400, 0x520) //AWB R GAIN
             || write_reg16(sensor->slv_addr, 0x3402, 0x410) //AWB G GAIN
             || write_reg16(sensor->slv_addr, 0x3404, 0x660);//AWB B GAIN
      break;
    case 4://HOME
      ret  = write_reg16(sensor->slv_addr, 0x3400, 0x420) //AWB R GAIN
             || write_reg16(sensor->slv_addr, 0x3402, 0x3f0) //AWB G GAIN
             || write_reg16(sensor->slv_addr, 0x3404, 0x710);//AWB B GAIN
      break;
    default://AUTO
      break;
  }

  if (ret == 0) {
    ESP_LOGD(TAG, "Set wb_mode to: %d", mode);
    sensor->status.wb_mode = mode;
  }
  return ret;
}

static int set_awb_gain_dsp(sensor_t *sensor, int enable)
{
  int ret = 0;
  int old_mode = sensor->status.wb_mode;
  int mode = enable ? old_mode : 0;

  ret = set_wb_mode(sensor, mode);

  if (ret == 0) {
    sensor->status.wb_mode = old_mode;
    ESP_LOGD(TAG, "Set awb_gain to: %d", enable);
    sensor->status.awb_gain = enable;
  }
  return ret;
}

static int set_special_effect(sensor_t *sensor, int effect)
{
  int ret = 0;
  if (effect < 0 || effect > 6) {
    return -1;
  }

  uint8_t * regs = (uint8_t *)ov5642_sensor_special_effects[effect];
  ret =  write_reg(sensor->slv_addr, 0x5580, regs[0])
         || write_reg(sensor->slv_addr, 0x5583, regs[1])
         || write_reg(sensor->slv_addr, 0x5584, regs[2])
         || write_reg(sensor->slv_addr, 0x5003, regs[3]);

  if (ret == 0) {
    ESP_LOGD(TAG, "Set special_effect to: %d", effect);
    sensor->status.special_effect = effect;
  }
  return ret;
}

static int set_brightness(sensor_t *sensor, int level)
{
  int ret = 0;
  uint8_t value = 0;
  bool negative = false;

  switch (level) {
    case 3:
      value = 0x30;
      break;
    case 2:
      value = 0x20;
      break;
    case 1:
      value = 0x10;
      break;
    case -1:
      value = 0x10;
      negative = true;
      break;
    case -2:
      value = 0x20;
      negative = true;
      break;
    case -3:
      value = 0x30;
      negative = true;
      break;
    default: // 0
      break;
  }

  ret = write_reg(sensor->slv_addr, 0x5587, value);
  if (ret == 0) {
    ret = write_reg_bits(sensor->slv_addr, 0x5588, 0x08, negative);
  }

  if (ret == 0) {
    ESP_LOGD(TAG, "Set brightness to: %d", level);
    sensor->status.brightness = level;
  }
  return ret;
}

static int set_contrast(sensor_t *sensor, int level)
{
  int ret = 0;
  if (level > 3 || level < -3) {
    return -1;
  }
  ret = write_reg(sensor->slv_addr, 0x5586, (level + 4) << 3);

  if (ret == 0) {
    ESP_LOGD(TAG, "Set contrast to: %d", level);
    sensor->status.contrast = level;
  }
  return ret;
}

static int set_saturation(sensor_t *sensor, int level)
{
  int ret = 0;
  if (level > 4 || level < -4) {
    return -1;
  }

  uint8_t * regs = (uint8_t *)ov5642_sensor_saturation_levels[level + 4];
  for (int i = 0; i < 11; i++) {
    ret = write_reg(sensor->slv_addr, 0x5381 + i, regs[i]);
    if (ret) {
      break;
    }
  }

  if (ret == 0) {
    ESP_LOGD(TAG, "Set saturation to: %d", level);
    sensor->status.saturation = level;
  }
  return ret;
}

static int set_sharpness(sensor_t *sensor, int level)
{
  int ret = 0;
  if (level > 3 || level < -3) {
    return -1;
  }

  uint8_t mt_offset_2 = (level + 3) * 8;
  uint8_t mt_offset_1 = mt_offset_2 + 1;

  ret = write_reg_bits(sensor->slv_addr, 0x5308, 0x40, false)//0x40 means auto
        || write_reg(sensor->slv_addr, 0x5300, 0x10)
        || write_reg(sensor->slv_addr, 0x5301, 0x10)
        || write_reg(sensor->slv_addr, 0x5302, mt_offset_1)
        || write_reg(sensor->slv_addr, 0x5303, mt_offset_2)
        || write_reg(sensor->slv_addr, 0x5309, 0x10)
        || write_reg(sensor->slv_addr, 0x530a, 0x10)
        || write_reg(sensor->slv_addr, 0x530b, 0x04)
        || write_reg(sensor->slv_addr, 0x530c, 0x06);

  if (ret == 0) {
    ESP_LOGD(TAG, "Set sharpness to: %d", level);
    sensor->status.sharpness = level;
  }
  return ret;
}

static int set_gainceiling(sensor_t *sensor, gainceiling_t level)
{
  int ret = 0, l = (int)level;

  ret = write_reg(sensor->slv_addr, 0x3A18, (l >> 8) & 3)
        || write_reg(sensor->slv_addr, 0x3A19, l & 0xFF);

  if (ret == 0) {
    ESP_LOGD(TAG, "Set gainceiling to: %d", l);
    sensor->status.gainceiling = l;
  }
  return ret;
}

static int get_denoise(sensor_t *sensor)
{
  if (!check_reg_mask(sensor->slv_addr, 0x5308, 0x10)) {
    return 0;
  }
  return (read_reg(sensor->slv_addr, 0x5306) / 4) + 1;
}

static int set_denoise(sensor_t *sensor, int level)
{
  int ret = 0;
  if (level < 0 || level > 8) {
    return -1;
  }

  ret = write_reg_bits(sensor->slv_addr, 0x5308, 0x10, level > 0);
  if (ret == 0 && level > 0) {
    ret = write_reg(sensor->slv_addr, 0x5306, (level - 1) * 4);
  }

  if (ret == 0) {
    ESP_LOGD(TAG, "Set denoise to: %d", level);
    sensor->status.denoise = level;
  }
  return ret;
}

static int init_status(sensor_t *sensor) {
  sensor->status.brightness = 0;
  sensor->status.contrast = 0;
  sensor->status.saturation = 0;
  sensor->status.sharpness = (read_reg(sensor->slv_addr, 0x5303) / 8) - 3;
  sensor->status.denoise = get_denoise(sensor);
  sensor->status.ae_level = 0;
  sensor->status.gainceiling = read_reg16(sensor->slv_addr, 0x3A18) & 0x3FF;
  sensor->status.awb = check_reg_mask(sensor->slv_addr, ISP_CONTROL_01, 0x01);
  sensor->status.dcw = !check_reg_mask(sensor->slv_addr, 0x5183, 0x80);
  sensor->status.agc = !check_reg_mask(sensor->slv_addr, AEC_PK_MANUAL, AEC_PK_MANUAL_AGC_MANUALEN);
  sensor->status.aec = !check_reg_mask(sensor->slv_addr, AEC_PK_MANUAL, AEC_PK_MANUAL_AEC_MANUALEN);
  sensor->status.hmirror = check_reg_mask(sensor->slv_addr, TIMING_TC_REG18, 0x40);
  sensor->status.vflip = check_reg_mask(sensor->slv_addr, TIMING_TC_REG18, 0x20);
  sensor->status.colorbar = check_reg_mask(sensor->slv_addr, PRE_ISP_TEST_SETTING_1, TEST_COLOR_BAR);
  sensor->status.bpc = check_reg_mask(sensor->slv_addr, 0x5000, 0x04);
  sensor->status.wpc = check_reg_mask(sensor->slv_addr, 0x5000, 0x02);
  sensor->status.raw_gma = check_reg_mask(sensor->slv_addr, 0x5000, 0x20);
  sensor->status.lenc = check_reg_mask(sensor->slv_addr, 0x5000, 0x80);
  sensor->status.quality = read_reg(sensor->slv_addr, COMPRESSION_CTRL07) & 0x3f;
  sensor->status.special_effect = 0;
  sensor->status.wb_mode = 0;
  sensor->status.awb_gain = check_reg_mask(sensor->slv_addr, 0x3406, 0x01);
  sensor->status.agc_gain = get_agc_gain(sensor);
  sensor->status.aec_value = get_aec_value(sensor);
  sensor->status.aec2 = check_reg_mask(sensor->slv_addr, 0x3a00, 0x04);
  return 0;
}

int ov5642_init(sensor_t *sensor)
{
  sensor->reset = reset;
  sensor->set_pixformat = set_pixformat;
  sensor->set_framesize = set_framesize;
  sensor->set_contrast = set_contrast;
  sensor->set_brightness = set_brightness;
  sensor->set_saturation = set_saturation;
  sensor->set_sharpness = set_sharpness;
  sensor->set_gainceiling = set_gainceiling;
  sensor->set_quality = set_quality;
  sensor->set_colorbar = set_colorbar;
  sensor->set_gain_ctrl = set_gain_ctrl;
  sensor->set_exposure_ctrl = set_exposure_ctrl;
  sensor->set_whitebal = set_whitebal;
  sensor->set_hmirror = set_hmirror;
  sensor->set_vflip = set_vflip;
  sensor->init_status = init_status;
  sensor->set_aec2 = set_aec2;
  sensor->set_aec_value = set_aec_value;
  sensor->set_special_effect = set_special_effect;
  sensor->set_wb_mode = set_wb_mode;
  sensor->set_ae_level = set_ae_level;
  sensor->set_dcw = set_dcw_dsp;
  sensor->set_bpc = set_bpc_dsp;
  sensor->set_wpc = set_wpc_dsp;
  sensor->set_awb_gain = set_awb_gain_dsp;
  sensor->set_agc_gain = set_agc_gain;
  sensor->set_raw_gma = set_raw_gma_dsp;
  sensor->set_lenc = set_lenc_dsp;
  sensor->set_denoise = set_denoise;
  return 0;
}
