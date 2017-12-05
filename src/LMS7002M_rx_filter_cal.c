///
/// \file LMS7002M_rx_filter_cal.c
///
/// Rx filter calibration functions for the LMS7002M C driver.
///
/// \copyright
/// Copyright (c) 2016-2017 Fairwaves, Inc.
/// Copyright (c) 2016-2016 Rice University
/// Copyright (c) 2016-2017 Skylark Wireless
/// SPDX-License-Identifier: Apache-2.0
/// http://www.apache.org/licenses/LICENSE-2.0
///

#include "LMS7002M_impl.h"
#include "LMS7002M_filter_cal.h"
#include <LMS7002M/LMS7002M_logger.h>
#include <LMS7002M/LMS7002M_time.h>
#include <string.h> //memcpy

/***********************************************************************
 * Re-tune the RX LO based on the bandwidth
 **********************************************************************/
static int setup_rx_cal_tone(LMS7002M_t *self, const LMS7002M_chan_t channel, const double bw,
                             const double sxr_extra_off)
{
    LMS7002M_sxx_enable(self, LMS_RX, true);
    LMS7002M_sxt_to_sxr(self, false);
    int status = 0;
    LMS7002M_set_mac_ch(self, channel);
    const double sxr_freq = self->sxt_freq-bw-sxr_extra_off;
    double sxr_freq_actual = 0;
    status = LMS7002M_set_lo_freq(self, LMS_RX, self->sxr_fref, sxr_freq, &sxr_freq_actual);
    LMS7002M_set_mac_ch(self, channel);
    if (status != 0)
    {
        LMS7_logf(LMS7_ERROR, self, "LMS7002M_set_lo_freq(LMS_RX, %f MHz)", sxr_freq/1e6);
        return status;
    }
    double rxtsp_rate = self->cgen_freq/4;
    const double rx_nco_freq = bw;
    LMS7002M_rxtsp_set_freq(self, channel, rx_nco_freq/rxtsp_rate);

    return status;
}

typedef enum cal_result {
    CAL_OK,
    CAL_LOW,  // coudn't satisfy desired RSSI -- rssi < desired_rssi
    CAL_HIGH, // coudn't satisfy desired RSSI -- rssi > desired_rssi
} cal_result_t;

/***********************************************************************
 * Rx calibration loop
 **********************************************************************/
static cal_result_t rx_cal_loop_inner(
        LMS7002M_t *self, const LMS7002M_chan_t channel,
        int *reg_ptr, const int reg_addr, const int reg_max, const char *reg_name,
        int desired_rssi_value)
{
    //--- binary search ----//
    int rssi_value;
    int best_lo = -1;
    int best_hi = -1;
    int range = (reg_max + 1) / 2;
    int initial = *reg_ptr;

    *reg_ptr = range;
    do {
        LMS7002M_regs_spi_write(self, reg_addr);
        rssi_value = cal_read_rssi(self, channel);

        LMS7_logf(LMS7_FATAL, self, "RSSI: [%c] %d -- %d (range %d) (val: %d)",
                  channel,
                  desired_rssi_value,
                  rssi_value,
                  range,
                  *reg_ptr);

        if (rssi_value < desired_rssi_value) {
            best_lo = *reg_ptr ;
            *reg_ptr -= range/2;
        } else {
            best_hi = *reg_ptr;
            *reg_ptr += range/2;
        }

        // TODO check clamping exit conditiohns
        if (*reg_ptr < 0)
            *reg_ptr = 0;
        else if (*reg_ptr > reg_max)
            *reg_ptr = reg_max;

        range /= 2;
    } while (range != 0);


    LMS7_logf(LMS7_FATAL, self, "RSSI: %d -- %d [%d %d] <= %d",
              desired_rssi_value,
              rssi_value, best_lo, best_hi, initial);

    LMS7_logf(LMS7_DEBUG, self, "%s = %d", reg_name, *reg_ptr);

    if (best_lo == -1)
        return CAL_LOW;
    else if (best_hi == -1)
        return CAL_HIGH;
    return CAL_OK;
}

static int rx_cal_loop(
        LMS7002M_t *self, const LMS7002M_chan_t channel, const double bw,
        int *reg_ptr, const int reg_addr, const int reg_max, const char *reg_name,
        int desired_rssi_value)
{
    LMS7002M_set_mac_ch(self, channel);

    int status = setup_rx_cal_tone(self, channel, bw, 50e3);
    if (status != 0) return -1;

    //--- calibration ---//
    unsigned r_range = 8;
    for (;;) {
        cal_result_t cres;
        cres = rx_cal_loop_inner(self, channel, reg_ptr, reg_addr,
                                 reg_max, reg_name, desired_rssi_value);

        if (cres == CAL_OK) {
            return 0;
        } else if (cres == CAL_LOW) {
            if (LMS7002M_regs(self)->reg_0x0116_r_ctl_lpf_rbb == 0 || r_range == 0)
                return -1;
            LMS7002M_regs(self)->reg_0x0116_r_ctl_lpf_rbb -= r_range;
        } else if (cres == CAL_HIGH) {
            if (LMS7002M_regs(self)->reg_0x0116_r_ctl_lpf_rbb == 31 || r_range == 0)
                return -1;
            LMS7002M_regs(self)->reg_0x0116_r_ctl_lpf_rbb += r_range;
        } else {
            return  -1;
        }

        r_range /= 2;

        if (LMS7002M_regs(self)->reg_0x0116_r_ctl_lpf_rbb < 0)
            LMS7002M_regs(self)->reg_0x0116_r_ctl_lpf_rbb = 0;
        else if (LMS7002M_regs(self)->reg_0x0116_r_ctl_lpf_rbb > 31)
            LMS7002M_regs(self)->reg_0x0116_r_ctl_lpf_rbb = 31;
        LMS7002M_regs_spi_write(self, 0x0116);

        LMS7_logf(LMS7_ERROR, self, "%s R: %d",
                  reg_name,
                  LMS7002M_regs(self)->reg_0x0116_r_ctl_lpf_rbb);
    }
}


/***********************************************************************
 * Prepare for RX filter self-calibration
 **********************************************************************/
static int rx_cal_init(LMS7002M_t *self, const LMS7002M_chan_t channel)
{
    int status = 0;
    LMS7002M_set_mac_ch(self, channel);
    const int g_tia_rfe_user = LMS7002M_regs(self)->reg_0x0113_g_tia_rfe;

    //--- rfe ---
    set_addrs_to_default(self, channel, 0x010C, 0x0114);
    LMS7002M_regs(self)->reg_0x010d_sel_path_rfe = 2;
    LMS7002M_regs(self)->reg_0x0113_g_rxloopb_rfe = 8;  // Gmax - 5
    LMS7002M_regs(self)->reg_0x010c_pd_rloopb_2_rfe = 0;
    LMS7002M_regs(self)->reg_0x010d_en_inshsw_lb2_rfe = 0;

    LMS7002M_regs(self)->reg_0x010d_en_inshsw_lb1_rfe = 1;
    LMS7002M_regs(self)->reg_0x010d_en_inshsw_l_rfe = 1;
    LMS7002M_regs(self)->reg_0x010d_en_inshsw_w_rfe = 1;

    LMS7002M_regs(self)->reg_0x010c_pd_mxlobuf_rfe = 0;
    LMS7002M_regs(self)->reg_0x010c_pd_qgen_rfe = 0;
    LMS7002M_regs(self)->reg_0x010f_ict_tiamain_rfe = 2;
    LMS7002M_regs(self)->reg_0x010f_ict_tiaout_rfe = 2;
    LMS7002M_regs(self)->reg_0x0114_rfb_tia_rfe = 16;
    LMS7002M_regs(self)->reg_0x0113_g_tia_rfe = g_tia_rfe_user;
    LMS7002M_regs_spi_write(self, 0x0113);
    LMS7002M_regs_spi_write(self, 0x0114);
    LMS7002M_regs_spi_write(self, 0x010c);
    LMS7002M_regs_spi_write(self, 0x010d);
    LMS7002M_regs_spi_write(self, 0x010f);

    //--- rbb ---
    set_addrs_to_default(self, channel, 0x0115, 0x011B);
    LMS7002M_regs(self)->reg_0x0119_ict_pga_out_rbb = 20;
    LMS7002M_regs(self)->reg_0x0119_ict_pga_in_rbb = 20;
    LMS7002M_regs(self)->reg_0x011a_c_ctl_pga_rbb = 3;
    LMS7002M_regs_spi_write(self, 0x0119);
    LMS7002M_regs_spi_write(self, 0x011a);

    //--- trf ---
    set_addrs_to_default(self, channel, 0x0100, 0x0104);
    LMS7002M_regs(self)->reg_0x0101_l_loopb_txpad_trf = 0;
    LMS7002M_regs(self)->reg_0x0101_en_loopb_txpad_trf = 1;
    LMS7002M_regs(self)->reg_0x0103_sel_band1_trf = 0;
    LMS7002M_regs(self)->reg_0x0103_sel_band2_trf = 1;
    LMS7002M_regs_spi_write(self, 0x0100);
    LMS7002M_regs_spi_write(self, 0x0101);
    LMS7002M_regs_spi_write(self, 0x0103);

    //--- tbb ---
    set_addrs_to_default(self, channel, 0x0105, 0x010B);
    LMS7002M_regs(self)->reg_0x0108_cg_iamp_tbb = 1;
    LMS7002M_regs(self)->reg_0x0108_ict_iamp_frp_tbb = 1;
    LMS7002M_regs(self)->reg_0x0108_ict_iamp_gg_frp_tbb = 6;
    LMS7002M_regs_spi_write(self, 0x0108);

    //--- rfe and trf nextrx -- must write to chA ---//
    LMS7002M_set_mac_ch(self, LMS_CHA);
    LMS7002M_regs(self)->reg_0x010d_en_nextrx_rfe = (channel == LMS_CHA)?0:1;
    LMS7002M_regs(self)->reg_0x0100_en_nexttx_trf = (channel == LMS_CHA)?0:1;
    LMS7002M_regs_spi_write(self, 0x010d);
    LMS7002M_regs_spi_write(self, 0x0100);
    LMS7002M_set_mac_ch(self, channel);

    //--- afe ---
    LMS7002M_afe_enable(self, LMS_RX, channel, true);
    LMS7002M_afe_enable(self, LMS_TX, channel, true);
    LMS7002M_set_mac_ch(self, channel);

    //--- bias -- must write to chA ---//
    LMS7002M_set_mac_ch(self, LMS_CHA);
    const int rp_calib_bias = LMS7002M_regs(self)->reg_0x0084_rp_calib_bias;
    set_addrs_to_default(self, channel, 0x0083, 0x0084);
    LMS7002M_regs(self)->reg_0x0084_rp_calib_bias = rp_calib_bias;
    LMS7002M_set_mac_ch(self, channel);

    //--- sxt ---
    const double sxt_freq = 550e6;
    status = LMS7002M_set_lo_freq(self, LMS_TX, self->sxt_fref, sxt_freq, NULL);
    LMS7002M_set_mac_ch(self, channel);
    if (status != 0)
    {
        LMS7_logf(LMS7_ERROR, self, "LMS7002M_set_lo_freq(LMS_TX, %f MHz)", sxt_freq/1e6);
        goto done;
    }

    //--- TxTSP ---
    set_addrs_to_default(self, channel, 0x0200, 0x020c);
    LMS7002M_regs(self)->reg_0x0200_tsgmode = 1;
    LMS7002M_regs(self)->reg_0x0200_insel = 1;
    LMS7002M_regs(self)->reg_0x0208_cmix_byp = 1;
    LMS7002M_regs(self)->reg_0x0208_gfir3_byp = 1;
    LMS7002M_regs(self)->reg_0x0208_gfir2_byp = 1;
    LMS7002M_regs(self)->reg_0x0208_gfir1_byp = 1;
    LMS7002M_regs_spi_write(self, 0x0200);
    LMS7002M_regs_spi_write(self, 0x0208);
    LMS7002M_txtsp_tsg_const(self, channel, 0x7fff, 0x8000);

    //--- RxTSP ---
    set_addrs_to_default(self, channel, 0x0400, 0x040f);
    LMS7002M_regs(self)->reg_0x040a_agc_mode = 1;
    LMS7002M_regs(self)->reg_0x040c_gfir3_byp = 1;
    LMS7002M_regs(self)->reg_0x040c_gfir2_byp = 1;
    LMS7002M_regs(self)->reg_0x040c_gfir1_byp = 1;
    LMS7002M_regs(self)->reg_0x040a_agc_avg = 12; //7
    LMS7002M_regs(self)->reg_0x040c_cmix_gain = 1;
    LMS7002M_regs_spi_write(self, 0x040a);
    LMS7002M_regs_spi_write(self, 0x040c);

    //--- initial cal tone ---//
//    status = setup_rx_cal_tone(self, channel, 10e3);
//    if (status != 0) goto done;

    done:
    return status;
}

/***********************************************************************
 * Perform RFE TIA filter calibration [0.5; 54] Mhz IF
 **********************************************************************/
static int rx_cal_tia_rfe(LMS7002M_t *self, const LMS7002M_chan_t channel,
                          const double bw, int rssi_value_50k)
{
    int status = 0;
    LMS7002M_set_mac_ch(self, channel);
    const int g_tia_rfe_user = LMS7002M_regs(self)->reg_0x0113_g_tia_rfe;
    int cfb_tia_rfe = 0;
    int ccomp_tia_rfe = 0;
    int rcomp_tia_rfe;

    //--- cfb_tia_rfe, ccomp_tia_rfe ---//
    if (g_tia_rfe_user == 3 || g_tia_rfe_user == 2)
    {
        cfb_tia_rfe = (int)(1680e6/bw - 10);
        ccomp_tia_rfe = cfb_tia_rfe/100;
    }
    else if (g_tia_rfe_user == 1)
    {
        cfb_tia_rfe = (int)(5400e6/bw - 10);
        ccomp_tia_rfe = (int)(cfb_tia_rfe/100 + 1);
    }
    else
    {
        LMS7_logf(LMS7_ERROR, self, "g_tia_rfe must be [1, 2, or 3], got %d", g_tia_rfe_user);
        status = -1;
        goto done;
    }

    if (ccomp_tia_rfe > 15) ccomp_tia_rfe = 15;
    LMS7002M_regs(self)->reg_0x0112_cfb_tia_rfe = cfb_tia_rfe;
    LMS7002M_regs(self)->reg_0x0112_ccomp_tia_rfe = ccomp_tia_rfe;
    LMS7002M_regs_spi_write(self, 0x0112);

    //--- rcomp_tia_rfe ---//
    rcomp_tia_rfe = (int)(15-2*cfb_tia_rfe/100);
    if (rcomp_tia_rfe < 0) rcomp_tia_rfe = 0;
    LMS7002M_regs(self)->reg_0x0114_rcomp_tia_rfe = rcomp_tia_rfe;
    LMS7002M_regs_spi_write(self, 0x0114);

    //--- rbb path ---//
    LMS7002M_regs(self)->reg_0x0118_input_ctl_pga_rbb = 2; // bypassing the LPF* blocks
    LMS7002M_regs(self)->reg_0x0115_pd_lpfl_rbb = 1; // power down LPFL block
    LMS7002M_regs(self)->reg_0x0115_pd_lpfh_rbb = 1; // power down LPFH block
    LMS7002M_regs_spi_write(self, 0x0118);
    LMS7002M_regs_spi_write(self, 0x0115);

    if (bw <= 0.5e6) {
        LMS7002M_regs(self)->reg_0x0112_cfb_tia_rfe = 4095;
        LMS7002M_regs_spi_write(self, 0x0112);
        goto done;
    }
    else if (bw > 54e6) {
        LMS7002M_regs(self)->reg_0x0112_ccomp_tia_rfe = 0;
        LMS7002M_regs(self)->reg_0x0112_cfb_tia_rfe = 0;
        LMS7002M_regs_spi_write(self, 0x0112);
        goto done;
    }

    status = setup_rx_cal_tone(self, channel, bw, 50e3);
    if (status != 0) return -1;

    //--- calibration ---//
    rx_cal_loop_inner(self, channel,
        &LMS7002M_regs(self)->reg_0x0112_cfb_tia_rfe,
        0x0112, 4095, "cfb_tia_rfe", (int)(rssi_value_50k*0.7071));

    done:
    return status;
}

/***********************************************************************
 * Perform RBB LPFL filter calibration
 **********************************************************************/
static int rx_cal_rbb_lpfl(LMS7002M_t *self, const LMS7002M_chan_t channel,
                           const double bw, int rssi_value_50k)
{
    int rcc_ctl_lpfl_rbb = 0;
    LMS7002M_set_mac_ch(self, channel);

    //--- c_ctl_lpfl_rbb, rcc_ctl_lpfl_rbb ---//
    LMS7002M_regs(self)->reg_0x0117_c_ctl_lpfl_rbb = (int)(2160e6/bw - 103);
    if      (bw > 15e6)  rcc_ctl_lpfl_rbb = 5;
    else if (bw > 10e6)  rcc_ctl_lpfl_rbb = 4;
    else if (bw > 5e6)   rcc_ctl_lpfl_rbb = 3;
    else if (bw > 3e6)   rcc_ctl_lpfl_rbb = 2;
    else if (bw > 1.4e6) rcc_ctl_lpfl_rbb = 1;
    else                 rcc_ctl_lpfl_rbb = 0;

    LMS7002M_regs(self)->reg_0x0115_pd_lpfh_rbb = 1;
    LMS7002M_regs(self)->reg_0x0115_pd_lpfl_rbb = 0; // power up LPFL block
    LMS7002M_regs(self)->reg_0x0116_r_ctl_lpf_rbb = 16;
    LMS7002M_regs(self)->reg_0x0117_rcc_ctl_lpfl_rbb = rcc_ctl_lpfl_rbb;
    LMS7002M_regs(self)->reg_0x0118_input_ctl_pga_rbb = 0;
    LMS7002M_regs_spi_write(self, 0x0115);
    LMS7002M_regs_spi_write(self, 0x0116);
    LMS7002M_regs_spi_write(self, 0x0117);
    LMS7002M_regs_spi_write(self, 0x0118);

#if 0
    //--- tia rfe registers ---//
    LMS7002M_regs(self)->reg_0x0112_cfb_tia_rfe = 1;
    LMS7002M_regs(self)->reg_0x0112_ccomp_tia_rfe = 0;
    LMS7002M_regs(self)->reg_0x0114_rcomp_tia_rfe = 15;
    LMS7002M_regs(self)->reg_0x0113_g_tia_rfe = 1;
    LMS7002M_regs_spi_write(self, 0x0112);
    LMS7002M_regs_spi_write(self, 0x0113);
    LMS7002M_regs_spi_write(self, 0x0114);
#endif

    if (bw <= 0.5e6) {
        // No need to tune set the best possible filtration
        LMS7002M_regs(self)->reg_0x0116_r_ctl_lpf_rbb = 0;
        LMS7002M_regs(self)->reg_0x0117_c_ctl_lpfl_rbb = 2047;
        LMS7002M_regs_spi_write(self, 0x0116);
        LMS7002M_regs_spi_write(self, 0x0117);
        return 0;
    }

    return rx_cal_loop(self, channel, bw,
                       &LMS7002M_regs(self)->reg_0x0117_c_ctl_lpfl_rbb,
                       0x0117, 2047, "c_ctl_lpfl_rbb", (int)(rssi_value_50k*0.7071));
}

/***********************************************************************
 * Perform RBB LPFH filter calibration
 **********************************************************************/
static int rx_cal_rbb_lpfh(LMS7002M_t *self, const LMS7002M_chan_t channel,
                           const double bw, int rssi_value_50k)
{
    LMS7002M_set_mac_ch(self, channel);

    //--- check filter bounds ---//
    if (bw < 20e6 || bw > 130e6)
    {
        LMS7_logf(LMS7_ERROR, self, "LPFH bandwidth not in range[20 to 130 MHz]");
        return -1;
    }

    //--- c_ctl_lpfl_rbb, rcc_ctl_lpfl_rbb ---//
    LMS7002M_regs(self)->reg_0x0116_c_ctl_lpfh_rbb = (int)(6000e6/bw - 50);
    int rcc_ctl_lpfh_rbb = (int)(bw/10e6 - 3);
    if (rcc_ctl_lpfh_rbb < 0) rcc_ctl_lpfh_rbb = 0;
    LMS7002M_regs(self)->reg_0x0116_rcc_ctl_lpfh_rbb = rcc_ctl_lpfh_rbb;
    LMS7002M_regs_spi_write(self, 0x0116);

    //--- tia rfe registers and rbb ---//
#if 0
    LMS7002M_regs(self)->reg_0x0112_cfb_tia_rfe = 15;
    LMS7002M_regs(self)->reg_0x0112_ccomp_tia_rfe = 1;
    LMS7002M_regs(self)->reg_0x0114_rcomp_tia_rfe = 15;
    LMS7002M_regs(self)->reg_0x0113_g_tia_rfe = 1;
#endif
    LMS7002M_regs(self)->reg_0x0115_pd_lpfh_rbb = 0;
	LMS7002M_regs(self)->reg_0x0115_pd_lpfl_rbb = 1;
    LMS7002M_regs(self)->reg_0x0118_input_ctl_pga_rbb = 1;
#if 0
    LMS7002M_regs_spi_write(self, 0x0112);
    LMS7002M_regs_spi_write(self, 0x0113);
    LMS7002M_regs_spi_write(self, 0x0114);
#endif
    LMS7002M_regs_spi_write(self, 0x0115);
    LMS7002M_regs_spi_write(self, 0x0118);

    LMS7002M_regs(self)->reg_0x0116_r_ctl_lpf_rbb = 16;
    LMS7002M_regs_spi_write(self, 0x0116);

    // CHECK ME!!
    int status = setup_rx_cal_tone(self, channel, 4e5, 1e5);
    if (status != 0) return status;

    rssi_value_50k = cal_gain_selection(self, channel, 0x05000);

    LMS7_logf(LMS7_ERROR, self, "LPFH ini %d C=%d", rssi_value_50k,
              LMS7002M_regs(self)->reg_0x0116_c_ctl_lpfh_rbb);

    //--- calibration ---//
    return rx_cal_loop(self, channel, bw,
                       &LMS7002M_regs(self)->reg_0x0116_c_ctl_lpfh_rbb,
                       0x0116, 255, "c_ctl_lpfl_rbb", (int)(rssi_value_50k*0.7071));
}

/***********************************************************************
 * Rx calibration dispatcher
 **********************************************************************/
int LMS7002M_rbb_set_filter_bw(LMS7002M_t *self, const LMS7002M_chan_t channel, double rfbw, double *bwactual)
{
    LMS7002M_set_mac_ch(self, channel);
    int status = 0;
    double bw = rfbw / 2;
    if (bw < 0.5) bw = 0.5; //low-band starts at 0.5
    const int path = (bw < 20e6) ? LMS7002M_RBB_LBF : LMS7002M_RBB_HBF;

    //check for initialized reference frequencies
    if (self->cgen_fref == 0.0)
    {
        LMS7_log(LMS7_ERROR, self, "cgen_fref not initialized");
        return -1;
    }
    if (self->sxr_fref == 0.0)
    {
        LMS7_log(LMS7_ERROR, self, "sxr_fref not initialized");
        return -1;
    }
    if (self->sxt_fref == 0.0)
    {
        LMS7_log(LMS7_ERROR, self, "sxt_fref not initialized");
        return -1;
    }

    ////////////////////////////////////////////////////////////////////
    // Save register map
    ////////////////////////////////////////////////////////////////////
    LMS7002M_regs_t saved_map[2];
    memcpy(saved_map, self->_regs, sizeof(saved_map));

    ////////////////////////////////////////////////////////////////////
    // Clocking configuration
    ////////////////////////////////////////////////////////////////////

    const uint16_t saturation_level = 0x05000; //-3dBFS
    int rssi_value_50k;

    if (bw > 0.5e6) {
        status = cal_setup_cgen(self, bw);
        if (status != 0)
        {
            LMS7_log(LMS7_ERROR, self, "cal_setup_cgen() faled");
            goto done;
        }

        ////////////////////////////////////////////////////////////////////
        // Load initial calibration state
        ////////////////////////////////////////////////////////////////////
        status = rx_cal_init(self, channel);
        if (status != 0)
        {
            LMS7_log(LMS7_ERROR, self, "rx_cal_init() failed");
            goto done;
        }
     }

     LMS7002M_regs(self)->reg_0x0112_cfb_tia_rfe = 1;
     LMS7002M_regs(self)->reg_0x0112_ccomp_tia_rfe = 0;
     LMS7002M_regs(self)->reg_0x0114_rcomp_tia_rfe = 15;
     LMS7002M_regs(self)->reg_0x0113_g_tia_rfe = 1;
     LMS7002M_regs(self)->reg_0x0118_input_ctl_pga_rbb = 2;
     LMS7002M_regs_spi_write(self, 0x0112);
     LMS7002M_regs_spi_write(self, 0x0113);
     LMS7002M_regs_spi_write(self, 0x0114);
     LMS7002M_regs_spi_write(self, 0x0118);

    // Find unfiltered RSSI
    if (bw > 0.5e6) {
        status = setup_rx_cal_tone(self, channel, 4e5, 1e5);
        if (status != 0) goto done;

        rssi_value_50k = cal_gain_selection(self, channel, saturation_level);
    } else {
        rssi_value_50k = saturation_level;
    }

    ////////////////////////////////////////////////////////////////////
    // RFE TIA calibration
    ////////////////////////////////////////////////////////////////////
    status = rx_cal_tia_rfe(self, channel, bw, rssi_value_50k * 1.26);
    if (status != 0)
    {
        LMS7_log(LMS7_ERROR, self, "rx_cal_tia_rfe() failed");
    }

    ////////////////////////////////////////////////////////////////////
    // RBB LPF calibration
    ////////////////////////////////////////////////////////////////////
    if (path == LMS7002M_RBB_LBF) status = rx_cal_rbb_lpfl(self, channel, bw, rssi_value_50k);
    if (path == LMS7002M_RBB_HBF) status = rx_cal_rbb_lpfh(self, channel, bw, rssi_value_50k);
    if (status != 0)
    {
        LMS7_log(LMS7_ERROR, self, "rx_cal_rbb_lpf() failed");
        goto done;
    }

    done:

    ////////////////////////////////////////////////////////////////////
    // stash tia + rbb calibration results
    ////////////////////////////////////////////////////////////////////
    LMS7002M_set_mac_ch(self, channel);
    const int cfb_tia_rfe = LMS7002M_regs(self)->reg_0x0112_cfb_tia_rfe;
    const int ccomp_tia_rfe = LMS7002M_regs(self)->reg_0x0112_ccomp_tia_rfe;
    const int rcomp_tia_rfe = LMS7002M_regs(self)->reg_0x0114_rcomp_tia_rfe;
    const int rcc_ctl_lpfl_rbb = LMS7002M_regs(self)->reg_0x0117_rcc_ctl_lpfl_rbb;
    const int c_ctl_lpfl_rbb = LMS7002M_regs(self)->reg_0x0117_c_ctl_lpfl_rbb;
    const int rcc_ctl_lpfh_rbb = LMS7002M_regs(self)->reg_0x0116_rcc_ctl_lpfh_rbb;
    const int c_ctl_lpfh_rbb = LMS7002M_regs(self)->reg_0x0116_c_ctl_lpfh_rbb;
    const int r_ctl_lpf_rbb = LMS7002M_regs(self)->reg_0x0116_r_ctl_lpf_rbb;

    ////////////////////////////////////////////////////////////////////
    // restore original register values
    ////////////////////////////////////////////////////////////////////
    memcpy(self->_regs, saved_map, sizeof(saved_map));
    LMS7002M_regs_to_rfic(self);
    LMS7002M_set_mac_ch(self, channel);

    ////////////////////////////////////////////////////////////////////
    // apply tia calibration results
    ////////////////////////////////////////////////////////////////////
    LMS7002M_set_mac_ch(self, channel);
    LMS7002M_regs(self)->reg_0x010f_ict_tiamain_rfe = 2;
    LMS7002M_regs(self)->reg_0x010f_ict_tiaout_rfe = 2;
    LMS7002M_regs(self)->reg_0x0114_rfb_tia_rfe = 16;
    LMS7002M_regs(self)->reg_0x0112_cfb_tia_rfe = cfb_tia_rfe;
    LMS7002M_regs(self)->reg_0x0112_ccomp_tia_rfe = ccomp_tia_rfe;
    LMS7002M_regs(self)->reg_0x0114_rcomp_tia_rfe = rcomp_tia_rfe;
    LMS7002M_regs_spi_write(self, 0x010f);
    LMS7002M_regs_spi_write(self, 0x0114);
    LMS7002M_regs_spi_write(self, 0x0112);

    ////////////////////////////////////////////////////////////////////
    // apply rbb calibration results
    ////////////////////////////////////////////////////////////////////
    LMS7002M_regs(self)->reg_0x0117_rcc_ctl_lpfl_rbb = rcc_ctl_lpfl_rbb;
    LMS7002M_regs(self)->reg_0x0117_c_ctl_lpfl_rbb = c_ctl_lpfl_rbb;
    LMS7002M_regs(self)->reg_0x0116_rcc_ctl_lpfh_rbb = rcc_ctl_lpfh_rbb;
    LMS7002M_regs(self)->reg_0x0116_c_ctl_lpfh_rbb = c_ctl_lpfh_rbb;
    LMS7002M_regs(self)->reg_0x0116_r_ctl_lpf_rbb = r_ctl_lpf_rbb;
    LMS7002M_regs(self)->reg_0x0119_ict_pga_out_rbb = 20;
    LMS7002M_regs(self)->reg_0x0119_ict_pga_in_rbb = 20;
    LMS7002M_regs_spi_write(self, 0x0117);
    LMS7002M_regs_spi_write(self, 0x0119);
    LMS7002M_regs_spi_write(self, 0x0116);

    ////////////////////////////////////////////////////////////////////
    // set the filter selection
    ////////////////////////////////////////////////////////////////////
    LMS7002M_rbb_set_path(self, channel, path);

    if (bwactual != NULL) *bwactual = bw;
    return status;
}
