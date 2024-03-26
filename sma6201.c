/* sma6201.c -- sma6201 ALSA SoC Audio driver
 *
 * r008, 2019.11.28	- initial version  sma6201
 *
 * Copyright 2023 Iron Device Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/version.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <asm/div64.h>
#include <linux/interrupt.h>
#include <linux/of_gpio.h>
#include <linux/thermal.h>
#include <linux/power_supply.h>
#include <linux/kfifo.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include "sma6201.h"

#define CHECK_COMP_PERIOD_TIME 10 /* sec per HZ */
#define CHECK_FAULT_PERIOD_TIME 5 /* sec per HZ */
#define DELAYED_SHUTDOWN_TIME 3 /* sec per HZ */

#define FIFO_BUFFER_SIZE 10
#define VBAT_TABLE_NUM 4

#define PLL_DEFAULT_SET 1

#define PLL_MATCH(_input_clk_name, _output_clk_name, _input_clk,\
		_post_n, _n, _f1, _f2, _f3_p_cp)\
{\
	.input_clk_name		= _input_clk_name,\
	.output_clk_name	= _output_clk_name,\
	.input_clk		= _input_clk,\
	.post_n			= _post_n,\
	.n			= _n,\
	.f1			= _f1,\
	.f2			= _f2,\
	.f3_p_cp		= _f3_p_cp,\
}

#define TEMP_GAIN_MATCH(_thermal_deg_name, _thermal_limit, _comp_gain,\
		_ocp_count, _hit_count, _activate)\
{\
	.thermal_deg_name	= _thermal_deg_name,\
	.thermal_limit		= _thermal_limit,\
	.comp_gain		= _comp_gain,\
	.ocp_count		= _ocp_count,\
	.hit_count		= _hit_count,\
	.activate		= _activate,\
}

#define VBAT_GAIN_MATCH(_vbat_level_name, _vbat_level,\
		_comp_gain)\
{\
	.lvl_name		= _vbat_level_name,\
	.vbat_level		= _vbat_level,\
	.comp_gain		= _comp_gain,\
}

enum sma6201_type {
	SMA6201,
};

/* PLL clock setting Table */
struct sma6201_pll_match {
	char *input_clk_name;
	char *output_clk_name;
	unsigned int input_clk;
	unsigned int post_n;
	unsigned int n;
	unsigned int f1;
	unsigned int f2;
	unsigned int f3_p_cp;
};

struct outside_status {
	unsigned int id;
	int thermal_deg;
	int batt_voltage_mV;
	int interval;
};

struct sma6201_temperature_match {
	char *thermal_deg_name;
	int thermal_limit;
	int comp_gain;
	unsigned int ocp_count;
	unsigned int hit_count;
	bool activate;
};

struct sma6201_vbat_gain_match {
	char *lvl_name;
	int vbat_level;
	int comp_gain;
};

struct sma6201_priv {
	enum sma6201_type devtype;
	struct attribute_group *attr_grp;
	struct kobject *kobj;
	struct regmap *regmap;
	struct sma6201_pll_match *pll_matches;
	struct wakeup_source shutdown_wakesrc;
	int num_of_pll_matches;
	struct sma6201_temperature_match *temp_match;
	int num_of_temperature_matches;
	unsigned int mclk_in;
	unsigned int sys_clk_id;
	unsigned int init_vol;
	unsigned int last_rate;
	unsigned int last_width;
	unsigned int last_channel;
	bool amp_power_status;
	bool ext_clk_status;
	bool force_amp_power_down;
	bool stereo_two_chip;
	bool src_bypass;
	unsigned int voice_music_class_h_mode;
	const uint32_t *eq1_reg_array;
	const uint32_t *eq2_reg_array;
	const uint32_t *bo_reg_array;
	uint32_t eq_reg_array_len;
	uint32_t bo_reg_array_len;
	unsigned int format;
	struct device *dev;
	struct delayed_work check_thermal_vbat_work;
	struct delayed_work check_thermal_fault_work;
	struct delayed_work delayed_shutdown_work;
	int irq;
	int gpio_int;
	int gpio_reset;
	unsigned int rev_num;
	atomic_t irq_enabled;
	unsigned int ocp_count;
	struct thermal_zone_device *tz_sense;
	struct power_supply *batt_psy;
	struct kfifo data_fifo;
	int fifo_count;
	struct mutex lock;
	uint32_t threshold_level;
	long check_thermal_vbat_period;
	long check_thermal_vbat_enable;
	long check_thermal_fault_period;
	long check_thermal_fault_enable;
	long delayed_shutdown_enable;
	long delayed_time_shutdown;
	long temp_table_number;
	long temp_limit;
	long temp_comp_gain;
	long temp_ocp_count;
	long temp_hit_count;
	long temp_activate;
	long enable_ocp_aging;
	long thermal_sense_opt;
	int lowbattery_status;
};

static struct sma6201_pll_match sma6201_pll_matches[] = {
/* in_clk_name, out_clk_name, input_clk, post_n, n, f1, f2, f3_p_cp */
PLL_MATCH("1.411MHz",  "24.595MHz", 1411200,  0x07, 0xF4, 0x00, 0x00, 0x03),
PLL_MATCH("1.536MHz",  "24.576MHz", 1536000,  0x07, 0xE0, 0x00, 0x00, 0x03),
PLL_MATCH("3.072MHz",  "24.576MHz", 3072000,  0x07, 0x70, 0x00, 0x00, 0x03),
PLL_MATCH("6.144MHz",  "24.576MHz", 6144000,  0x07, 0x70, 0x00, 0x00, 0x07),
PLL_MATCH("12.288MHz", "24.576MHz", 12288000, 0x07, 0x70, 0x00, 0x00, 0x0B),
PLL_MATCH("19.2MHz",   "24.343MHz", 19200000, 0x07, 0x47, 0x00, 0x00, 0x0A),
PLL_MATCH("24.576MHz", "24.576MHz", 24576000, 0x07, 0x70, 0x00, 0x00, 0x0F),
};

static const struct sma6201_vbat_gain_match sma6201_vbat_gain_matches[] = {
/* level name,level, comp gain*/
VBAT_GAIN_MATCH("Normal LVL", 3, 0x00),
VBAT_GAIN_MATCH("LVL 2", 2, 0x02),
VBAT_GAIN_MATCH("LVL 1", 1, 0x04),
VBAT_GAIN_MATCH("LVL 0", 0, 0x06),
};

#ifndef CONFIG_MACH_PIEZO
static struct sma6201_temperature_match sma6201_temperature_gain_matches[] = {
/* degree name, temp limit, comp gain, ocp count, hit count, activate */
TEMP_GAIN_MATCH("35", 350, 0x00, 0, 0, 1), /* normal */
TEMP_GAIN_MATCH("40", 400, 0x01, 0, 0, 1),
TEMP_GAIN_MATCH("45", 450, 0x02, 0, 0, 1),
TEMP_GAIN_MATCH("50", 500, 0x03, 0, 0, 1),
TEMP_GAIN_MATCH("55", 550, 0x04, 0, 0, 1),
TEMP_GAIN_MATCH("60", 600, 0x05, 0, 0, 1),
TEMP_GAIN_MATCH("65", 650, 0x06, 0, 0, 1),
TEMP_GAIN_MATCH("70", 700, 0x07, 0, 0, 1),
TEMP_GAIN_MATCH("75", 750, 0x08, 0, 0, 1),
TEMP_GAIN_MATCH("80", 800, 0x09, 0, 0, 1),
TEMP_GAIN_MATCH("85", 850, 0x0a, 0, 0, 1),
TEMP_GAIN_MATCH("90", 900, 0x0b, 0, 0, 1),
TEMP_GAIN_MATCH("95", 950, 0x0c, 0, 0, 1),
TEMP_GAIN_MATCH("100", 1000, 0xd, 0, 0, 1), /* max */
};
#else
static struct sma6201_temperature_match sma6201_temperature_gain_matches[] = {
/* degree name, temp limit, comp gain, ocp count, hit count, activate */
TEMP_GAIN_MATCH("42.5", 425, 0x00, 0, 0, 0), /* normal */
TEMP_GAIN_MATCH("48.8", 488, 0x01, 0, 0, 0),
TEMP_GAIN_MATCH("55.0", 550, 0x02, 0, 0, 1),
TEMP_GAIN_MATCH("61.3", 613, 0x03, 0, 0, 1),
TEMP_GAIN_MATCH("67.5", 675, 0x04, 0, 0, 1),
TEMP_GAIN_MATCH("73.8", 738, 0x05, 0, 0, 1),
TEMP_GAIN_MATCH("80.0", 800, 0x06, 0, 0, 1),
TEMP_GAIN_MATCH("86.3", 863, 0x07, 0, 0, 1),
TEMP_GAIN_MATCH("92.5", 925, 0x08, 0, 0, 1),
TEMP_GAIN_MATCH("98.8", 988, 0x09, 0, 0, 1),
TEMP_GAIN_MATCH("100.0", 1000, 0xd, 0, 0, 1), /* max */
};
#endif

static int sma6201_startup(struct snd_soc_component *);
static int sma6201_shutdown(struct snd_soc_component *);
static int sma6201_thermal_compensation(struct sma6201_priv *sma6201,
					bool ocp_status);

/* Initial register value - {register, value}
 * EQ Band : 1 to 10 / 0x40 to 0x8A (15EA register for each EQ Band)
 * Currently all EQ Bands are flat frequency response
 */
static const struct reg_default sma6201_reg_def[] = {
	{ 0x00, 0x80 }, /* 0x00 SystemCTRL  */
	{ 0x01, 0x00 }, /* 0x01 InputCTRL1  */
	{ 0x02, 0x00 }, /* 0x02 InputCTRL2  */
	{ 0x03, 0x01 }, /* 0x03 InputCTRL3  */
	{ 0x04, 0x41 }, /* 0x04 PIEZO_FILTER_TUNING  */
	{ 0x05, 0xBA }, /* 0x05 BrownOut Set1  */
	{ 0x06, 0x7A }, /* 0x06 BrownOut Set2  */
	{ 0x07, 0x3A }, /* 0x07 BrownOut Set3  */
	{ 0x08, 0x2A }, /* 0x08 BrownOut Set4  */
	{ 0x09, 0x00 }, /* 0x09 OutputCTRL  */
	{ 0x0A, 0x58 }, /* 0x0A SPK_VOL  */
	{ 0x0B, 0x1A }, /* 0x0B BrownOut Set5  */
	{ 0x0C, 0x0A }, /* 0x0C BrownOut Set6  */
	{ 0x0D, 0xC2 }, /* 0x0D Class-H Control Level1  */
	{ 0x0E, 0xAF }, /* 0x0E MUTE_VOL_CTRL  */
	{ 0x0F, 0xA2 }, /* 0x0F Class-H Control Level2  */
	{ 0x10, 0x00 }, /* 0x10 SystemCTRL1  */
	{ 0x11, 0x00 }, /* 0x11 SystemCTRL2  */
	{ 0x12, 0x00 }, /* 0x12 SystemCTRL3  */
	{ 0x13, 0x28 }, /* 0x13 FDPEC Control1  */
	{ 0x14, 0x60 }, /* 0x14 Modulator  */
	{ 0x15, 0x01 }, /* 0x15 BassSpk1  */
	{ 0x16, 0x0F }, /* 0x16 BassSpk2  */
	{ 0x17, 0x0F }, /* 0x17 BassSpk3  */
	{ 0x18, 0x0F }, /* 0x18 BassSpk4  */
	{ 0x19, 0x00 }, /* 0x19 BassSpk5  */
	{ 0x1A, 0x00 }, /* 0x1A BassSpk6  */
	{ 0x1B, 0x00 }, /* 0x1B BassSpk7  */
	{ 0x1C, 0xC0 }, /* 0x1C BrownOut Protection16  */
	{ 0x1D, 0xB3 }, /* 0x1D BrownOut Protection17  */
	{ 0x1E, 0xA6 }, /* 0x1E BrownOut Protection18  */
	{ 0x1F, 0x99 }, /* 0x1F BrownOut Protection19  */
	{ 0x20, 0x00 }, /* 0x20 BrownOut Protection20  */
	{ 0x21, 0x80 }, /* 0x21 DGC  */
	{ 0x22, 0x31 }, /* 0x22 Prescaler  */
	{ 0x23, 0x19 }, /* 0x23 CompLim1  */
	{ 0x24, 0x00 }, /* 0x24 CompLim2  */
	{ 0x25, 0x00 }, /* 0x25 CompLim3  */
	{ 0x26, 0x04 }, /* 0x26 CompLim4  */
	{ 0x27, 0x8C }, /* 0x27 RET_CUR_CTRL  */
	{ 0x28, 0x8A }, /* 0x28 Class-H Control Level3  */
	{ 0x29, 0xC9 }, /* 0x29 Class-H Control Level4  */
	{ 0x2A, 0x88 }, /* 0x2A Class-H Control Level5  */
	{ 0x2B, 0x07 }, /* 0x2B EqMode  */
	{ 0x2C, 0x0C }, /* 0x2C EqBand1_BYP  */
	{ 0x2D, 0x0C }, /* 0x2D EqBand2_BYP  */
	{ 0x2E, 0x0C }, /* 0x2E EqBand3_BYP  */
	{ 0x2F, 0x0C }, /* 0x2F EqBand4_BYP  */
	{ 0x30, 0x0C }, /* 0x30 EqBand5_BYP  */
	{ 0x33, 0x00 }, /* 0x33 SDM_CTRL  */
	{ 0x36, 0x92 }, /* 0x36 Protection  */
	{ 0x37, 0x3F }, /* 0x37 SlopeCTRL  */
	{ 0x38, 0x00 }, /* 0x38 DIS_CLASSH_LVL12  */
	{ 0x39, 0x88 }, /* 0x39 DIS_CLASSH_LVL34  */
	{ 0x3A, 0x8C }, /* 0x3A DIS_CLASSH_LVL56  */
	{ 0x3B, 0x00 }, /* 0x3B Test1  */
	{ 0x3C, 0x00 }, /* 0x3C Test2  */
	{ 0x3D, 0x00 }, /* 0x3D Test3  */
	{ 0x3E, 0x03 }, /* 0x3E ATEST1  */
	{ 0x3F, 0x00 }, /* 0x3F ATEST2  */
	{ 0x40, 0x00 }, /* 0x40 EQCTRL1 : EQ BAND1 */
	{ 0x41, 0x00 }, /* 0x41 EQCTRL2  */
	{ 0x42, 0x00 }, /* 0x42 EQCTRL3  */
	{ 0x43, 0x00 }, /* 0x43 EQCTRL4  */
	{ 0x44, 0x00 }, /* 0x44 EQCTRL5  */
	{ 0x45, 0x00 }, /* 0x45 EQCTRL6  */
	{ 0x46, 0x20 }, /* 0x46 EQCTRL7  */
	{ 0x47, 0x00 }, /* 0x47 EQCTRL8  */
	{ 0x48, 0x00 }, /* 0x48 EQCTRL9  */
	{ 0x49, 0x00 }, /* 0x49 EQCTRL10  */
	{ 0x4A, 0x00 }, /* 0x4A EQCTRL11  */
	{ 0x4B, 0x00 }, /* 0x4B EQCTRL12  */
	{ 0x4C, 0x00 }, /* 0x4C EQCTRL13  */
	{ 0x4D, 0x00 }, /* 0x4D EQCTRL14  */
	{ 0x4E, 0x00 }, /* 0x4E EQCTRL15  */
	{ 0x4F, 0x00 }, /* 0x4F EQCTRL16 : EQ BAND2 */
	{ 0x50, 0x00 }, /* 0x50 EQCTRL17  */
	{ 0x51, 0x00 }, /* 0x51 EQCTRL18  */
	{ 0x52, 0x00 }, /* 0x52 EQCTRL19  */
	{ 0x53, 0x00 }, /* 0x53 EQCTRL20  */
	{ 0x54, 0x00 }, /* 0x54 EQCTRL21  */
	{ 0x55, 0x20 }, /* 0x55 EQCTRL22  */
	{ 0x56, 0x00 }, /* 0x56 EQCTRL23  */
	{ 0x57, 0x00 }, /* 0x57 EQCTRL24  */
	{ 0x58, 0x00 }, /* 0x58 EQCTRL25  */
	{ 0x59, 0x00 }, /* 0x59 EQCTRL26  */
	{ 0x5A, 0x00 }, /* 0x5A EQCTRL27  */
	{ 0x5B, 0x00 }, /* 0x5B EQCTRL28  */
	{ 0x5C, 0x00 }, /* 0x5C EQCTRL29  */
	{ 0x5D, 0x00 }, /* 0x5D EQCTRL30  */
	{ 0x5E, 0x00 }, /* 0x5E EQCTRL31 : EQ BAND3 */
	{ 0x5F, 0x00 }, /* 0x5F EQCTRL32  */
	{ 0x60, 0x00 }, /* 0x60 EQCTRL33  */
	{ 0x61, 0x00 }, /* 0x61 EQCTRL34  */
	{ 0x62, 0x00 }, /* 0x62 EQCTRL35  */
	{ 0x63, 0x00 }, /* 0x63 EQCTRL36  */
	{ 0x64, 0x20 }, /* 0x64 EQCTRL37  */
	{ 0x65, 0x00 }, /* 0x65 EQCTRL38  */
	{ 0x66, 0x00 }, /* 0x66 EQCTRL39  */
	{ 0x67, 0x00 }, /* 0x67 EQCTRL40  */
	{ 0x68, 0x00 }, /* 0x68 EQCTRL41  */
	{ 0x69, 0x00 }, /* 0x69 EQCTRL42  */
	{ 0x6A, 0x00 }, /* 0x6A EQCTRL43  */
	{ 0x6B, 0x00 }, /* 0x6B EQCTRL44  */
	{ 0x6C, 0x00 }, /* 0x6C EQCTRL45  */
	{ 0x6D, 0x00 }, /* 0x6D EQCTRL46 : EQ BAND4 */
	{ 0x6E, 0x00 }, /* 0x6E EQCTRL47  */
	{ 0x6F, 0x00 }, /* 0x6F EQCTRL48  */
	{ 0x70, 0x00 }, /* 0x70 EQCTRL49  */
	{ 0x71, 0x00 }, /* 0x71 EQCTRL50  */
	{ 0x72, 0x00 }, /* 0x72 EQCTRL51  */
	{ 0x73, 0x20 }, /* 0x73 EQCTRL52  */
	{ 0x74, 0x00 }, /* 0x74 EQCTRL53  */
	{ 0x75, 0x00 }, /* 0x75 EQCTRL54  */
	{ 0x76, 0x00 }, /* 0x76 EQCTRL55  */
	{ 0x77, 0x00 }, /* 0x77 EQCTRL56  */
	{ 0x78, 0x00 }, /* 0x78 EQCTRL57  */
	{ 0x79, 0x00 }, /* 0x79 EQCTRL58  */
	{ 0x7A, 0x00 }, /* 0x7A EQCTRL59  */
	{ 0x7B, 0x00 }, /* 0x7B EQCTRL60  */
	{ 0x7C, 0x00 }, /* 0x7C EQCTRL61 : EQ BAND5 */
	{ 0x7D, 0x00 }, /* 0x7D EQCTRL62  */
	{ 0x7E, 0x00 }, /* 0x7E EQCTRL63  */
	{ 0x7F, 0x00 }, /* 0x7F EQCTRL64  */
	{ 0x80, 0x00 }, /* 0x80 EQCTRL65  */
	{ 0x81, 0x00 }, /* 0x81 EQCTRL66  */
	{ 0x82, 0x20 }, /* 0x82 EQCTRL67  */
	{ 0x83, 0x00 }, /* 0x83 EQCTRL68  */
	{ 0x84, 0x00 }, /* 0x84 EQCTRL69  */
	{ 0x85, 0x00 }, /* 0x85 EQCTRL70  */
	{ 0x86, 0x00 }, /* 0x86 EQCTRL71  */
	{ 0x87, 0x00 }, /* 0x87 EQCTRL72  */
	{ 0x88, 0x00 }, /* 0x88 EQCTRL73  */
	{ 0x89, 0x00 }, /* 0x89 EQCTRL74  */
	{ 0x8A, 0x00 }, /* 0x8A EQCTRL75  */
	{ 0x8B, 0x07 }, /* 0x8B PLL_POST_N  */
	{ 0x8C, 0x70 }, /* 0x8C PLL_N  */
	{ 0x8D, 0x00 }, /* 0x8D PLL_F1  */
	{ 0x8E, 0x00 }, /* 0x8E PLL_F2  */
	{ 0x8F, 0x03 }, /* 0x8F PLL_F3,P,CP  */
	{ 0x90, 0xC2 }, /* 0x90 Class-H Control Level6 */
	{ 0x91, 0x82 }, /* 0x91 Class-H Control Level7 */
	{ 0x92, 0x32 }, /* 0x92 FDPEC Control2  */
	{ 0x93, 0x8E }, /* 0x93 Boost Control0  */
	{ 0x94, 0x9B }, /* 0x94 Boost Control1  */
	{ 0x95, 0x25 }, /* 0x95 Boost Control2  */
	{ 0x96, 0x3E }, /* 0x96 Boost Control3  */
	{ 0x97, 0xE8 }, /* 0x97 Boost Control4  */
	{ 0x98, 0x49 }, /* 0x98 GeneralSetting  */
	{ 0x9A, 0xC0 }, /* 0x9A Volume_IADC  */
	{ 0x9C, 0x0C }, /* 0x9C Volume_PGA_ISENSE */
	{ 0x9D, 0xFF }, /* 0x9D ENABLE_ISENSE  */
	{ 0x9E, 0x6C }, /* 0x9E TRIM_ISENSE_Current_1  */
	{ 0x9F, 0x6D }, /* 0x9F TRIM_ISENSE_Current_2  */
	{ 0xA0, 0x80 }, /* 0xA0 ADC MUTE_VOL_CTRL	*/
	{ 0xA2, 0x68 }, /* 0xA2 TOP_MAN1  */
	{ 0xA3, 0x28 }, /* 0xA3 TOP_MAN2  */
	{ 0xA4, 0x46 }, /* 0xA4 SDO OUTPUT FORMAT  */
	{ 0xA5, 0x01 }, /* 0xA5 TDM1  */
	{ 0xA6, 0x41 }, /* 0xA6 TDM2  */
	{ 0xA7, 0x00 }, /* 0xA7 TOP_MAN3  */
	{ 0xA8, 0xA1 }, /* 0xA8 PIEZO / TONE GENERATOR  */
	{ 0xA9, 0x67 }, /* 0xA9 TONE / FINE VOLUME  */
	{ 0xAA, 0x8B }, /* 0xAA PLL_A_Setting  */
	{ 0xAB, 0x01 }, /* 0xAB PLL_D_Setting  */
	{ 0xAC, 0x2F }, /* 0xAC PLL_CTRL  */
	{ 0xAD, 0x09 }, /* 0xAD SPK_OCP_LVL  */
	{ 0xAE, 0x12 }, /* 0xAE TOP_MAN4  */
	{ 0xAF, 0xC0 }, /* 0xAF VIN_Sensing  */
	{ 0xB0, 0x08 }, /* 0xB0 Brown Out Protection0  */
	{ 0xB1, 0xAA }, /* 0xB1 Brown Out Protection1  */
	{ 0xB2, 0x99 }, /* 0xB2 Brown Out Protection2   */
	{ 0xB3, 0x8C }, /* 0xB3 Brown Out Protection3  */
	{ 0xB4, 0x1C }, /* 0xB4 Brown Out Protection4  */
	{ 0xB5, 0x1B }, /* 0xB5 Brown Out Protection5  */
	{ 0xB6, 0xE6 }, /* 0xB6 Brown Out Protection6  */
	{ 0xB7, 0xD9 }, /* 0xB7 Brown Out Protection7  */
	{ 0xB8, 0x7F }, /* 0xB8 Brown Out Protection8  */
	{ 0xB9, 0x76 }, /* 0xB9 Brown Out Protection9  */
	{ 0xBA, 0x6E }, /* 0xBA Brown Out Protection10  */
	{ 0xBB, 0x6A }, /* 0xBB Brown Out Protection11  */
	{ 0xBC, 0x18 }, /* 0xBC Brown Out Protection12  */
	{ 0xBD, 0x76 }, /* 0xBD Brown Out Protection13  */
	{ 0xBE, 0x94 }, /* 0xBE Brown Out Protection14  */
	{ 0xBF, 0xB3 }, /* 0xBF Brown Out Protection15  */
	{ 0xFA, 0xE0 }, /* 0xFA Status1  */
	{ 0xFB, 0x00 }, /* 0xFB Status2  */
	{ 0xFC, 0x00 }, /* 0xFC Status3  */
	{ 0xFD, 0x00 }, /* 0xFD Status4  */
	{ 0xFE, 0x00 }, /* 0xFE Status5  */
	{ 0xFF, 0xD0 }, /* 0xFF Device Info  */
};

static bool sma6201_readable_register(struct device *dev, unsigned int reg)
{
	if (reg > SMA6201_FF_VERSION)
		return false;

	switch (reg) {
	case SMA6201_00_SYSTEM_CTRL ... SMA6201_30_EQBAND5_BYP:
	case SMA6201_33_SDM_CTRL:
	case SMA6201_36_PROTECTION ... SMA6201_98_GENERAL_SETTING:
	case SMA6201_9A_VOLUME_IADC:
	case SMA6201_9C_VOLUME_PGA_ISENSE ... SMA6201_A0_ADC_MUTE_VOL_CTRL:
	case SMA6201_A2_TOP_MAN1 ... SMA6201_BF_BROWN_OUT_P15:
	case SMA6201_FA_STATUS1 ... SMA6201_FF_VERSION:
		return true;
	default:
		return false;
	}
}

static bool sma6201_writeable_register(struct device *dev, unsigned int reg)
{
	if (reg > SMA6201_FF_VERSION)
		return false;

	switch (reg) {
	case SMA6201_00_SYSTEM_CTRL ... SMA6201_30_EQBAND5_BYP:
	case SMA6201_33_SDM_CTRL:
	case SMA6201_36_PROTECTION ... SMA6201_98_GENERAL_SETTING:
	case SMA6201_9A_VOLUME_IADC:
	case SMA6201_9C_VOLUME_PGA_ISENSE ... SMA6201_A0_ADC_MUTE_VOL_CTRL:
	case SMA6201_A2_TOP_MAN1 ... SMA6201_BF_BROWN_OUT_P15:
	case SMA6201_FA_STATUS1 ... SMA6201_FF_VERSION:
		return true;
	default:
		return false;
	}
}

static bool sma6201_volatile_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case SMA6201_FA_STATUS1 ... SMA6201_FF_VERSION:
		return true;
	default:
		return false;
	}
}

/* DB scale conversion of speaker volume(mute:-60dB) */
static const DECLARE_TLV_DB_SCALE(sma6201_spk_tlv, -6000, 50, 0);

/* common bytes ext functions */
static int bytes_ext_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol, int reg)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	struct soc_bytes_ext *params = (void *)kcontrol->private_value;
	unsigned int i, reg_val;
	u8 *val;

	val = (u8 *)ucontrol->value.bytes.data;
	for (i = 0; i < params->max; i++) {
		regmap_read(sma6201->regmap, reg + i, &reg_val);
		if (sizeof(reg_val) > 2)
			reg_val = cpu_to_le32(reg_val);
		else
			reg_val = cpu_to_le16(reg_val);
		memcpy(val + i, &reg_val, sizeof(u8));
	}

	regmap_update_bits(sma6201->regmap, SMA6201_2B_EQ_MODE,
			EQ_BANK_SEL_MASK, EQ1_BANK_SEL);

	return 0;
}

static int bytes_ext_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol, int reg)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	struct soc_bytes_ext *params = (void *)kcontrol->private_value;
	void *data;
	u8 *val;
	int i, ret;

	data = kmemdup(ucontrol->value.bytes.data,
			params->max, GFP_KERNEL | GFP_DMA);
	if (!data)
		return -ENOMEM;

	val = (u8 *)data;
	for (i = 0; i < params->max; i++) {
		ret = regmap_write(sma6201->regmap, reg + i, *(val + i));
		if (ret) {
			dev_err(component->dev,
				"configuration fail, register: %x ret: %d\n",
				reg + i, ret);
			kfree(data);
			return ret;
		}
	}
	kfree(data);

	regmap_update_bits(sma6201->regmap, SMA6201_2B_EQ_MODE,
			EQ_BANK_SEL_MASK, EQ1_BANK_SEL);

	return 0;
}

static int power_up_down_control_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = sma6201->amp_power_status;

	return 0;
}

static int power_up_down_control_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 1))
		return -EINVAL;

	if (sel && !(sma6201->force_amp_power_down))
		sma6201_startup(component);
	else
		sma6201_shutdown(component);

	return 0;
}

static int power_down_control_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = sma6201->force_amp_power_down;

	return 0;
}

static int power_down_control_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	sma6201->force_amp_power_down = ucontrol->value.integer.value[0];

	if (sma6201->force_amp_power_down) {
		dev_info(component->dev, "%s\n", "Force AMP power down mode");
		sma6201_shutdown(component);
	} else
		dev_info(component->dev, "%s\n",
				"Force AMP power down out of mode");

	return 0;
}

/* Clock System Set */
static const char * const sma6201_clk_system_text[] = {
	"Reserved", "Reserved", "Reserved", "External clock 19.2MHz",
	"External clock 24.576MHz", "Reserved", "Reserved", "Reserved"};

static const struct soc_enum sma6201_clk_system_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_clk_system_text),
		sma6201_clk_system_text);

static int sma6201_clk_system_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_00_SYSTEM_CTRL, &val);
	ucontrol->value.integer.value[0] = ((val & 0xE0) >> 5);

	return 0;
}

static int sma6201_clk_system_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_00_SYSTEM_CTRL,
			0xE0, (sel << 5));

	return 0;
}

/* InputCTRL1 Set */
static const char * const sma6201_input_format_text[] = {
	"Philips standard I2S", "Left justified", "Not used",
	"Not used", "Right justified 16bits", "Right justified 18bits",
	"Right justified 20bits", "Right justified 24bits"};

static const struct soc_enum sma6201_input_format_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_input_format_text),
		sma6201_input_format_text);

static int sma6201_input_format_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_01_INPUT1_CTRL1, &val);
	ucontrol->value.integer.value[0] = ((val & 0x70) >> 4);

	return 0;
}

static int sma6201_input_format_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_01_INPUT1_CTRL1,
			0x70, (sel << 4));

	return 0;
}

/* Piezo Filter Tuning Set */
static int piezo_filter_tune_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_04_PIEZO_FILTER_TUNE);
}

static int piezo_filter_tune_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_04_PIEZO_FILTER_TUNE);
}

/* BrownOut Set 1 to 4 */
static int brown_out_set1_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_05_BROWNOUT_SET1);
}

static int brown_out_set1_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_05_BROWNOUT_SET1);
}

/* Input / output port config */
static const char * const sma6201_port_config_text[] = {
	"Input port only", "Reserved", "Output port enable", "Reserved"};

static const struct soc_enum sma6201_port_config_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_port_config_text),
	sma6201_port_config_text);

static int sma6201_port_config_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_09_OUTPUT_CTRL, &val);
	ucontrol->value.integer.value[0] = ((val & 0x60) >> 5);

	return 0;
}

static int sma6201_port_config_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap,
		SMA6201_09_OUTPUT_CTRL, 0x60, (sel << 5));

	return 0;
}

/* Output format select */
static const char * const sma6201_port_out_format_text[] = {
	"I2S 32 SCK", "I2S 64 SCK", "PCM short sync 128fs", "Reserved"};

static const struct soc_enum sma6201_port_out_format_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_port_out_format_text),
	sma6201_port_out_format_text);

static int sma6201_port_out_format_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_09_OUTPUT_CTRL, &val);
	ucontrol->value.integer.value[0] = ((val & 0x18) >> 3);

	return 0;
}

static int sma6201_port_out_format_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap,
		SMA6201_09_OUTPUT_CTRL, 0x18, (sel << 3));

	return 0;
}

/* Output source */
static const char * const sma6201_port_out_sel_text[] = {
	"Disable", "Format Converter", "Mixer output",
	"SPK path, EQ, Bass, Vol, DRC",
	"Modulator input/tone generator output for test",
	"Reserved", "Reserved", "Reserved"};

static const struct soc_enum sma6201_port_out_sel_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_port_out_sel_text),
	sma6201_port_out_sel_text);

static int sma6201_port_out_sel_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_09_OUTPUT_CTRL, &val);
	ucontrol->value.integer.value[0] = (val & 0x07);

	return 0;
}

static int sma6201_port_out_sel_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_09_OUTPUT_CTRL, 0x07, sel);

	return 0;
}

/* BrownOut Set 5 to 6 */
static int brown_out_set2_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_0B_BROWNOUT_SET5);
}

static int brown_out_set2_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_0B_BROWNOUT_SET5);
}

/* Class-H Control Level1 Set */
static const char * const sma6201_attack_lvl_1_text[] = {
	"BOOST_ON", "LVL_0.01562FS", "LVL_0.03125FS",
	"LVL_0.04688FS", "LVL_0.0625FS", "LVL_0.07813FS",
	"LVL_0.09376FS", "LVL_0.10938FS", "LVL_0.125FS",
	"LVL_0.14063FS", "LVL_0.15626FS", "LVL_0.17189FS",
	"LVL_0.18751FS", "LVL_0.20314FS", "LVL_0.21876FS",
	"BOOST_OFF"};

static const struct soc_enum sma6201_attack_lvl_1_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_attack_lvl_1_text),
		sma6201_attack_lvl_1_text);

static int sma6201_attack_lvl_1_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_0D_CLASS_H_CTRL_LVL1, &val);
	ucontrol->value.integer.value[0] = ((val & 0xF0) >> 4);

	return 0;
}

static int sma6201_attack_lvl_1_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_0D_CLASS_H_CTRL_LVL1,
			0xF0, (sel << 4));

	return 0;
}

static const char * const sma6201_release_time_1_text[] = {
	"Time_00ms", "Time_10ms", "Time_20ms", "Time_30ms",
	"Time_40ms", "Time_50ms", "Time_60ms", "Time_70ms",
	"Time_80ms", "Time_90ms", "Time_100ms", "Time_110ms",
	"Time_120ms", "Time_130ms", "Time_140ms", "Time_150ms"};

static const struct soc_enum sma6201_release_time_1_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_release_time_1_text),
		sma6201_release_time_1_text);

static int sma6201_release_time_1_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_0D_CLASS_H_CTRL_LVL1, &val);
	ucontrol->value.integer.value[0] = (val & 0x0F);

	return 0;
}

static int sma6201_release_time_1_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_0D_CLASS_H_CTRL_LVL1,
			0x0F, sel);

	return 0;
}

/* Volume slope */
static const char * const sma6201_vol_slope_text[] = {
	"Off", "Slow(about 1sec)", "Medium(about 0.5sec)",
	"Fast(about 0.1sec)"};

static const struct soc_enum sma6201_vol_slope_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_vol_slope_text),
	sma6201_vol_slope_text);

static int sma6201_vol_slope_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_0E_MUTE_VOL_CTRL, &val);
	ucontrol->value.integer.value[0] = ((val & 0xC0) >> 6);

	return 0;
}

static int sma6201_vol_slope_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap,
		SMA6201_0E_MUTE_VOL_CTRL, 0xC0, (sel << 6));

	return 0;
}

/* Mute slope */
static const char * const sma6201_mute_slope_text[] = {
	"Off", "Slow(about 200ms)", "Medium(about 50ms)",
	"Fast(about 10ms)"};

static const struct soc_enum sma6201_mute_slope_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_mute_slope_text),
	sma6201_mute_slope_text);

static int sma6201_mute_slope_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_0E_MUTE_VOL_CTRL, &val);
	ucontrol->value.integer.value[0] = ((val & 0x30) >> 4);

	return 0;
}

static int sma6201_mute_slope_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap,
		SMA6201_0E_MUTE_VOL_CTRL, 0x30, (sel << 4));

	return 0;
}

/* Class-H Control Level2 Set */
static const char * const sma6201_attack_lvl_2_text[] = {
	"BOOST_ON", "LVL_0.03125FS", "LVL_0.0625FS", "LVL_0.09375FS",
	"LVL_0.125FS", "LVL_0.15625FS", "LVL_0.1875FS", "LVL_0.21875FS",
	"LVL_0.25FS", "LVL_0.28125FS", "LVL_0.3125FS", "LVL_0.34375FS",
	"LVL_0.375FS", "LVL_0.40625FS", "LVL_0.4375FS", "BOOST_OFF"};

static const struct soc_enum sma6201_attack_lvl_2_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_attack_lvl_2_text),
		sma6201_attack_lvl_2_text);

static int sma6201_attack_lvl_2_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_0F_CLASS_H_CTRL_LVL2, &val);
	ucontrol->value.integer.value[0] = ((val & 0xF0) >> 4);

	return 0;
}

static int sma6201_attack_lvl_2_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_0F_CLASS_H_CTRL_LVL2,
			0xF0, (sel << 4));

	return 0;
}

static const char * const sma6201_release_time_2_text[] = {
	"Time_00ms", "Time_10ms", "Time_20ms", "Time_30ms",
	"Time_40ms", "Time_50ms", "Time_60ms", "Time_70ms",
	"Time_80ms", "Time_90ms", "Time_100ms", "Time_110ms",
	"Time_120ms", "Time_130ms", "Time_140ms", "Time_150ms"};

static const struct soc_enum sma6201_release_time_2_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_release_time_2_text),
		sma6201_release_time_2_text);

static int sma6201_release_time_2_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_0F_CLASS_H_CTRL_LVL2, &val);
	ucontrol->value.integer.value[0] = (val & 0x0F);

	return 0;
}

static int sma6201_release_time_2_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_0F_CLASS_H_CTRL_LVL2,
			0x0F, sel);

	return 0;
}

/* Speaker mode */
static const char * const sma6201_spkmode_text[] = {
	"Off", "Mono for one chip solution", "Reserved", "Reserved",
	"Stereo for two chip solution", "Reserved", "Reserved", "Reserved"};

static const struct soc_enum sma6201_spkmode_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_spkmode_text),
	sma6201_spkmode_text);

static int sma6201_spkmode_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_10_SYSTEM_CTRL1, &val);
	ucontrol->value.integer.value[0] = ((val & 0x1C) >> 2);

	return 0;
}

static int sma6201_spkmode_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap,
		SMA6201_10_SYSTEM_CTRL1, 0x1C, (sel << 2));

	if (sel == (SPK_MONO >> 2)) {
		sma6201->stereo_two_chip = false;
		dev_info(component->dev, "%s : Mono for one chip solution\n",
					__func__);
		if (sma6201->src_bypass == true)
			regmap_update_bits(sma6201->regmap, SMA6201_A3_TOP_MAN2,
				BP_SRC_MIX_MASK, BP_SRC_MIX_MONO);
	} else if (sel == (SPK_STEREO >> 2)) {
		sma6201->stereo_two_chip = true;
		dev_info(component->dev, "%s : Stereo for two chip solution\n",
					__func__);
		regmap_update_bits(sma6201->regmap, SMA6201_A3_TOP_MAN2,
			BP_SRC_MIX_MASK, BP_SRC_MIX_NORMAL);
		regmap_update_bits(sma6201->regmap,
			SMA6201_11_SYSTEM_CTRL2, MONOMIX_MASK, MONOMIX_OFF);
	}

	return 0;
}

/* SystemCTRL3 Set */
static const char * const sma6201_input_gain_text[] = {
	"Gain_0dB", "Gain_-6dB", "Gain_-12dB", "Gain_-Infinity"};

static const struct soc_enum sma6201_input_gain_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_input_gain_text),
		sma6201_input_gain_text);

static int sma6201_input_gain_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_12_SYSTEM_CTRL3, &val);
	ucontrol->value.integer.value[0] = ((val & 0xC0) >> 6);

	return 0;
}

static int sma6201_input_gain_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_12_SYSTEM_CTRL3,
			0xC0, (sel << 6));

	return 0;
}

static const char * const sma6201_input_r_gain_text[] = {
	"Gain_0dB", "Gain_-6dB", "Gain_-12dB", "Gain_-Infinity"};

static const struct soc_enum sma6201_input_r_gain_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_input_r_gain_text),
		sma6201_input_r_gain_text);

static int sma6201_input_r_gain_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_12_SYSTEM_CTRL3, &val);
	ucontrol->value.integer.value[0] = ((val & 0x30) >> 4);

	return 0;
}

static int sma6201_input_r_gain_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_12_SYSTEM_CTRL3,
			0x30, (sel << 4));

	return 0;
}

/* FDPEC Control1 Set */
static const char * const sma6201_fdpec_i_text[] = {
	"I_40uA", "I_80uA", "I_120uA", "I_160uA"};

static const struct soc_enum sma6201_fdpec_i_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_fdpec_i_text), sma6201_fdpec_i_text);

static int sma6201_fdpec_i_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_13_FDPEC_CTRL1, &val);
	ucontrol->value.integer.value[0] = ((val & 0x18) >> 3);

	return 0;
}

static int sma6201_fdpec_i_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_13_FDPEC_CTRL1,
			0x18, (sel << 3));

	return 0;
}

static const char * const fdpec_gain_control_text[] = {
	"Gain 2", "Gain 4", "Gain 8", "Gain 8", "Gain 1.5",
	"Gain 3", "Gain 6", "Gain 6"};

static const struct soc_enum fdpec_gain_control_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(fdpec_gain_control_text),
	fdpec_gain_control_text);

static int fdpec_gain_control_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_13_FDPEC_CTRL1, &val);
	ucontrol->value.integer.value[0] = (val & 0x07);

	return 0;
}

static int fdpec_gain_control_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_13_FDPEC_CTRL1,
			0x07, sel);

	return 0;
}


/* Modulator Set */
static const char * const sma6201_spk_hysfb_text[] = {
	"f_625kHz", "f_414kHz", "f_297kHz", "f_226kHz"};

static const struct soc_enum sma6201_spk_hysfb_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_spk_hysfb_text), sma6201_spk_hysfb_text);

static int sma6201_spk_hysfb_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_14_MODULATOR, &val);
	ucontrol->value.integer.value[0] = ((val & 0xC0) >> 6);

	return 0;
}

static int sma6201_spk_hysfb_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_14_MODULATOR,
			0xC0, (sel << 6));

	return 0;
}

static int spk_bdelay_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_14_MODULATOR);
}

static int spk_bdelay_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_14_MODULATOR);
}

/* bass boost speaker coeff */
static int bass_spk_coeff_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_15_BASS_SPK1);
}

static int bass_spk_coeff_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_15_BASS_SPK1);
}

/* Brown Out Protection 16-20 Set */
static int brown_out_pt2_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_1C_BROWN_OUT_P16);
}

static int brown_out_pt2_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_1C_BROWN_OUT_P16);
}

/* DGC(Dynamic Gain Control) Delay Set */
static int dgc_delay_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_21_DGC);
}

static int dgc_delay_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_21_DGC);
}

/* Prescaler Set */
static int prescaler_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_22_PRESCALER);
}

static int prescaler_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_22_PRESCALER);
}

/* DRC speaker coeff */
static int comp_lim_spk_coeff_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_23_COMP_LIM1);
}

static int comp_lim_spk_coeff_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_23_COMP_LIM1);
}

/* Class-H Control Level3 Set */
static const char * const sma6201_attack_lvl_3_text[] = {
	"BOOST_ON", "LVL_0.03125FS", "LVL_0.0625FS", "LVL_0.09375FS",
	"LVL_0.125FS", "LVL_0.15625FS", "LVL_0.1875FS", "LVL_0.21875FS",
	"LVL_0.25FS", "LVL_0.28125FS", "LVL_0.3125FS", "LVL_0.34375FS",
	"LVL_0.375FS", "LVL_0.40625FS", "LVL_0.4375FS", "BOOST_OFF"};

static const struct soc_enum sma6201_attack_lvl_3_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_attack_lvl_3_text),
		sma6201_attack_lvl_3_text);

static int sma6201_attack_lvl_3_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_28_CLASS_H_CTRL_LVL3, &val);
	ucontrol->value.integer.value[0] = ((val & 0xF0) >> 4);

	return 0;
}

static int sma6201_attack_lvl_3_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_28_CLASS_H_CTRL_LVL3,
			0xF0, (sel << 4));

	return 0;
}

static const char * const sma6201_release_time_3_text[] = {
	"Time_00ms", "Time_10ms", "Time_20ms", "Time_30ms",
	"Time_40ms", "Time_50ms", "Time_60ms", "Time_70ms",
	"Time_80ms", "Time_90ms", "Time_100ms", "Time_110ms",
	"Time_120ms", "Time_130ms", "Time_140ms", "Time_150ms"};

static const struct soc_enum sma6201_release_time_3_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_release_time_3_text),
		sma6201_release_time_3_text);

static int sma6201_release_time_3_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_28_CLASS_H_CTRL_LVL3, &val);
	ucontrol->value.integer.value[0] = (val & 0x0F);

	return 0;
}

static int sma6201_release_time_3_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_28_CLASS_H_CTRL_LVL3,
			0x0F, sel);

	return 0;
}

/* Class-H Control Level4 Set */
static const char * const sma6201_attack_lvl_4_text[] = {
	"BOOST_ON", "LVL_0.03125FS", "LVL_0.0625FS", "LVL_0.09375FS",
	"LVL_0.125FS", "LVL_0.15625FS", "LVL_0.1875FS", "LVL_0.21875FS",
	"LVL_0.25FS", "LVL_0.28125FS", "LVL_0.3125FS", "LVL_0.34375FS",
	"LVL_0.375FS", "LVL_0.40625FS", "LVL_0.4375FS", "BOOST_OFF"};

static const struct soc_enum sma6201_attack_lvl_4_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_attack_lvl_4_text),
		sma6201_attack_lvl_4_text);

static int sma6201_attack_lvl_4_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_29_CLASS_H_CTRL_LVL4, &val);
	ucontrol->value.integer.value[0] = ((val & 0xF0) >> 4);

	return 0;
}

static int sma6201_attack_lvl_4_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_29_CLASS_H_CTRL_LVL4,
			0xF0, (sel << 4));

	return 0;
}

static const char * const sma6201_release_time_4_text[] = {
	"Time_00ms", "Time_10ms", "Time_20ms", "Time_30ms",
	"Time_40ms", "Time_50ms", "Time_60ms", "Time_70ms",
	"Time_80ms", "Time_90ms", "Time_100ms", "Time_110ms",
	"Time_120ms", "Time_130ms", "Time_140ms", "Time_150ms"};

static const struct soc_enum sma6201_release_time_4_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_release_time_4_text),
		sma6201_release_time_4_text);

static int sma6201_release_time_4_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_29_CLASS_H_CTRL_LVL4, &val);
	ucontrol->value.integer.value[0] = (val & 0x0F);

	return 0;
}

static int sma6201_release_time_4_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_29_CLASS_H_CTRL_LVL4,
			0x0F, sel);

	return 0;
}

/* Class-H Control Level5 Set */
static const char * const sma6201_attack_lvl_5_text[] = {
	"BOOST_ON", "LVL_0.0625FS", "LVL_0.125FS", "LVL_0.1875FS",
	"LVL_0.25FS", "LVL_0.3125FS", "LVL_0.375FS", "LVL_0.4375FS",
	"LVL_0.5FS", "LVL_0.625FS", "LVL_0.6875FS", "LVL_0.75FS",
	"LVL_0.8125FS", "LVL_0.875FS", "LVL_0.9375FS", "BOOST_OFF"};

static const struct soc_enum sma6201_attack_lvl_5_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_attack_lvl_5_text),
		sma6201_attack_lvl_5_text);

static int sma6201_attack_lvl_5_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_2A_CLASS_H_CTRL_LVL5, &val);
	ucontrol->value.integer.value[0] = ((val & 0xF0) >> 4);

	return 0;
}

static int sma6201_attack_lvl_5_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_2A_CLASS_H_CTRL_LVL5,
			0xF0, (sel << 4));

	return 0;
}

static const char * const sma6201_release_time_5_text[] = {
	"Time_00ms", "Time_10ms", "Time_20ms", "Time_30ms",
	"Time_40ms", "Time_50ms", "Time_60ms", "Time_70ms",
	"Time_80ms", "Time_90ms", "Time_100ms", "Time_110ms",
	"Time_120ms", "Time_130ms", "Time_140ms", "Time_150ms"};

static const struct soc_enum sma6201_release_time_5_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_release_time_5_text),
		sma6201_release_time_5_text);

static int sma6201_release_time_5_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_2A_CLASS_H_CTRL_LVL5, &val);
	ucontrol->value.integer.value[0] = (val & 0x0F);

	return 0;
}

static int sma6201_release_time_5_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_2A_CLASS_H_CTRL_LVL5,
			0x0F, sel);

	return 0;
}

/* OTP MODE Set */
static const char * const sma6201_otp_mode_text[] = {
	"Disable", "Ignore threshold1, shutdown threshold2",
	"Reduced threshold1, shutdown threshold2",
	"Shutdown threshold1, shutdown threshold2"};

static const struct soc_enum sma6201_otp_mode_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_otp_mode_text), sma6201_otp_mode_text);

static int sma6201_otp_mode_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_36_PROTECTION, &val);
	ucontrol->value.integer.value[0] = (val & 0x03);

	return 0;
}

static int sma6201_otp_mode_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_36_PROTECTION, 0x03, sel);

	return 0;
}

/* Slope CTRL */
static int slope_ctrl_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_37_SLOPE_CTRL);
}

static int slope_ctrl_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_37_SLOPE_CTRL);
}

/* Disable class-H set */
static int dis_class_h_lvl_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_38_DIS_CLASSH_LVL12);
}

static int dis_class_h_lvl_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_38_DIS_CLASSH_LVL12);
}

/* Test 1~3, ATEST 1~2 */
static int test_mode_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_3B_TEST1);
}

static int test_mode_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_3B_TEST1);
}

/* EQ1 Band1 */
static int eq1_ctrl_band1_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_40_EQ_CTRL1);
}

static int eq1_ctrl_band1_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_40_EQ_CTRL1);
}

/* EQ1 Band2 */
static int eq1_ctrl_band2_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_4F_EQ_CTRL16);
}

static int eq1_ctrl_band2_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_4F_EQ_CTRL16);
}

/* EQ1 Band3 */
static int eq1_ctrl_band3_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_5E_EQ_CTRL31);
}

static int eq1_ctrl_band3_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_5E_EQ_CTRL31);
}

/* EQ1 Band4 */
static int eq1_ctrl_band4_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_6D_EQ_CTRL46);
}

static int eq1_ctrl_band4_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_6D_EQ_CTRL46);
}

/* EQ1 Band5 */
static int eq1_ctrl_band5_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_7C_EQ_CTRL61);
}

static int eq1_ctrl_band5_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_7C_EQ_CTRL61);
}

/* EQ2 Band1 */
static int eq2_ctrl_band1_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	regmap_update_bits(sma6201->regmap, SMA6201_2B_EQ_MODE,
			EQ_BANK_SEL_MASK, EQ2_BANK_SEL);

	return bytes_ext_get(kcontrol, ucontrol, SMA6201_40_EQ_CTRL1);
}

static int eq2_ctrl_band1_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	regmap_update_bits(sma6201->regmap, SMA6201_2B_EQ_MODE,
			EQ_BANK_SEL_MASK, EQ2_BANK_SEL);

	return bytes_ext_put(kcontrol, ucontrol, SMA6201_40_EQ_CTRL1);
}

/* EQ2 Band2 */
static int eq2_ctrl_band2_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	regmap_update_bits(sma6201->regmap, SMA6201_2B_EQ_MODE,
			EQ_BANK_SEL_MASK, EQ2_BANK_SEL);

	return bytes_ext_get(kcontrol, ucontrol, SMA6201_4F_EQ_CTRL16);
}

static int eq2_ctrl_band2_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	regmap_update_bits(sma6201->regmap, SMA6201_2B_EQ_MODE,
			EQ_BANK_SEL_MASK, EQ2_BANK_SEL);

	return bytes_ext_put(kcontrol, ucontrol, SMA6201_4F_EQ_CTRL16);
}

/* EQ2 Band3 */
static int eq2_ctrl_band3_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	regmap_update_bits(sma6201->regmap, SMA6201_2B_EQ_MODE,
			EQ_BANK_SEL_MASK, EQ2_BANK_SEL);

	return bytes_ext_get(kcontrol, ucontrol, SMA6201_5E_EQ_CTRL31);
}

static int eq2_ctrl_band3_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	regmap_update_bits(sma6201->regmap, SMA6201_2B_EQ_MODE,
			EQ_BANK_SEL_MASK, EQ2_BANK_SEL);

	return bytes_ext_put(kcontrol, ucontrol, SMA6201_5E_EQ_CTRL31);
}

/* EQ2 Band4 */
static int eq2_ctrl_band4_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	regmap_update_bits(sma6201->regmap, SMA6201_2B_EQ_MODE,
			EQ_BANK_SEL_MASK, EQ2_BANK_SEL);

	return bytes_ext_get(kcontrol, ucontrol, SMA6201_6D_EQ_CTRL46);
}

static int eq2_ctrl_band4_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	regmap_update_bits(sma6201->regmap, SMA6201_2B_EQ_MODE,
			EQ_BANK_SEL_MASK, EQ2_BANK_SEL);

	return bytes_ext_put(kcontrol, ucontrol, SMA6201_6D_EQ_CTRL46);
}

/* EQ2 Band5 */
static int eq2_ctrl_band5_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	regmap_update_bits(sma6201->regmap, SMA6201_2B_EQ_MODE,
			EQ_BANK_SEL_MASK, EQ2_BANK_SEL);

	return bytes_ext_get(kcontrol, ucontrol, SMA6201_7C_EQ_CTRL61);
}

static int eq2_ctrl_band5_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	regmap_update_bits(sma6201->regmap, SMA6201_2B_EQ_MODE,
			EQ_BANK_SEL_MASK, EQ2_BANK_SEL);

	return bytes_ext_put(kcontrol, ucontrol, SMA6201_7C_EQ_CTRL61);
}


/* PLL setting */
static int pll_setting_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_8B_PLL_POST_N);
}

static int pll_setting_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_8B_PLL_POST_N);
}

/* Class-H Control Level6 Set */
static const char * const sma6201_attack_lvl_6_text[] = {
	"BOOST_ON", "LVL_0.0625FS", "LVL_0.125FS", "LVL_0.1875FS",
	"LVL_0.25FS", "LVL_0.3125FS", "LVL_0.375FS", "LVL_0.4375FS",
	"LVL_0.5FS", "LVL_0.625FS", "LVL_0.6875FS", "LVL_0.75FS",
	"LVL_0.8125FS", "LVL_0.875FS", "LVL_0.9375FS", "BOOST_OFF"};

static const struct soc_enum sma6201_attack_lvl_6_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_attack_lvl_6_text),
		sma6201_attack_lvl_6_text);

static int sma6201_attack_lvl_6_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_90_CLASS_H_CTRL_LVL6, &val);
	ucontrol->value.integer.value[0] = ((val & 0xF0) >> 4);

	return 0;
}

static int sma6201_attack_lvl_6_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_90_CLASS_H_CTRL_LVL6,
			0xF0, (sel << 4));

	return 0;
}

static const char * const sma6201_release_time_6_text[] = {
	"Time_00ms", "Time_10ms", "Time_20ms", "Time_30ms",
	"Time_40ms", "Time_50ms", "Time_60ms", "Time_70ms",
	"Time_80ms", "Time_90ms", "Time_100ms", "Time_110ms",
	"Time_120ms", "Time_130ms", "Time_140ms", "Time_150ms"};

static const struct soc_enum sma6201_release_time_6_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_release_time_6_text),
		sma6201_release_time_6_text);

static int sma6201_release_time_6_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_90_CLASS_H_CTRL_LVL6, &val);
	ucontrol->value.integer.value[0] = (val & 0x0F);

	return 0;
}

static int sma6201_release_time_6_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_90_CLASS_H_CTRL_LVL6,
			0x0F, sel);

	return 0;
}

/* Class-H Control Level7 Set */
static const char * const sma6201_attack_lvl_7_text[] = {
	"BOOST_ON", "LVL_0.0625FS", "LVL_0.125FS", "LVL_0.1875FS",
	"LVL_0.25FS", "LVL_0.3125FS", "LVL_0.375FS", "LVL_0.4375FS",
	"LVL_0.5FS", "LVL_0.625FS", "LVL_0.6875FS", "LVL_0.75FS",
	"LVL_0.8125FS", "LVL_0.875FS", "LVL_0.9375FS", "BOOST_OFF"};

static const struct soc_enum sma6201_attack_lvl_7_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_attack_lvl_7_text),
		sma6201_attack_lvl_7_text);

static int sma6201_attack_lvl_7_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_91_CLASS_H_CTRL_LVL7, &val);
	ucontrol->value.integer.value[0] = ((val & 0xF0) >> 4);

	return 0;
}

static int sma6201_attack_lvl_7_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_91_CLASS_H_CTRL_LVL7,
			0xF0, (sel << 4));

	return 0;
}

static const char * const sma6201_release_time_7_text[] = {
	"Time_00ms", "Time_10ms", "Time_20ms", "Time_30ms",
	"Time_40ms", "Time_50ms", "Time_60ms", "Time_70ms",
	"Time_80ms", "Time_90ms", "Time_100ms", "Time_110ms",
	"Time_120ms", "Time_130ms", "Time_140ms", "Time_150ms"};

static const struct soc_enum sma6201_release_time_7_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_release_time_7_text),
		sma6201_release_time_7_text);

static int sma6201_release_time_7_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_91_CLASS_H_CTRL_LVL7, &val);
	ucontrol->value.integer.value[0] = (val & 0x0F);

	return 0;
}

static int sma6201_release_time_7_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_91_CLASS_H_CTRL_LVL7,
			0x0F, sel);

	return 0;
}

/* FDPEC Control2 Set */
static const char * const sma6201_fdpec_gain_trm_text[] = {
	"No trimming", "7% increase", "10% increase", "26% increase"};

static const struct soc_enum sma6201_fdpec_gain_trm_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_fdpec_gain_trm_text),
		sma6201_fdpec_gain_trm_text);

static int sma6201_fdpec_gain_trm_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_92_FDPEC_CTRL2, &val);
	ucontrol->value.integer.value[0] = ((val & 0xC0) >> 6);

	return 0;
}

static int sma6201_fdpec_gain_trm_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_92_FDPEC_CTRL2,
			0xC0, (sel << 6));

	return 0;
}

static const char * const sma6201_diffamp_i_text[] = {
	"I_40uA", "I_80uA", "I_120uA", "I_160uA"};

static const struct soc_enum sma6201_diffamp_i_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_diffamp_i_text), sma6201_diffamp_i_text);

static int sma6201_diffamp_i_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_92_FDPEC_CTRL2, &val);
	ucontrol->value.integer.value[0] = ((val & 0x06) >> 1);

	return 0;
}

static int sma6201_diffamp_i_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_92_FDPEC_CTRL2,
			0x06, (sel << 1));

	return 0;
}

/* Boost control0 Set*/
static const char * const sma6201_trm_vref_text[] = {
	"REF_1.3V", "REF_1.2875V", "REF_1.275V", "REF_1.2625V", "REF_1.25V",
	"REF_1.2375V", "REF_1.225V", "REF_1.2125V", "REF_1.2V", "REF_1.1875V",
	"REF_1.175V", "REF_1.1625V", "REF_1.15V", "REF_1.1375V", "REF_1.125V",
	"REF_1.1125V"};

static const struct soc_enum sma6201_trm_vref_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_trm_vref_text), sma6201_trm_vref_text);

static int sma6201_trm_vref_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_93_BOOST_CTRL0, &val);
	ucontrol->value.integer.value[0] = ((val & 0xF0) >> 4);

	return 0;
}

static int sma6201_trm_vref_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_93_BOOST_CTRL0,
			0xF0, (sel << 4));

	return 0;
}

static const char * const sma6201_trm_vbst1_text[] = {
	"BST_6V", "BST_7V", "BST_8V", "BST_9V", "BST_10V", "BST_11V",
	"BST_12V", "BST_13V", "BST_14V", "BST_15V", "BST_16V", "BST_17V",
	"BST_18V", "BST_19V", "BST_20V", "BST_21V"};

static const struct soc_enum sma6201_trm_vbst1_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_trm_vbst1_text), sma6201_trm_vbst1_text);

static int sma6201_trm_vbst1_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_93_BOOST_CTRL0, &val);
	ucontrol->value.integer.value[0] = (val & 0x0F);

	return 0;
}

static int sma6201_trm_vbst1_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	dev_info(component->dev, "%s : Trimming of boost output voltage %dV\n",
				__func__, (sel + 6));
	regmap_update_bits(sma6201->regmap, SMA6201_93_BOOST_CTRL0,
				0x0F, sel);

	return 0;
}

/* Boost control1 Set*/
static const char * const sma6201_trm_comp2_text[] = {
	"C_10pF", "C_30pF", "C_50pF", "C_70pF"};

static const struct soc_enum sma6201_trm_comp2_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_trm_comp2_text), sma6201_trm_comp2_text);

static int sma6201_trm_comp2_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_94_BOOST_CTRL1, &val);
	ucontrol->value.integer.value[0] = ((val & 0xC0) >> 6);

	return 0;
}

static int sma6201_trm_comp2_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_94_BOOST_CTRL1,
			0xC0, (sel << 6));

	return 0;
}

static const char * const sma6201_trm_osc_text[] = {
	"f_1.37MHz", "f_1.54MHz", "f_1.76MHz", "f_2.05MHz",
	"f_2.23MHz", "f_2.46MHz", "f_3.07MHz", "f_3.51MHz"};

static const struct soc_enum sma6201_trm_osc_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_trm_osc_text), sma6201_trm_osc_text);

static int sma6201_trm_osc_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_94_BOOST_CTRL1, &val);
	ucontrol->value.integer.value[0] = ((val & 0x38) >> 3);

	return 0;
}

static int sma6201_trm_osc_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_94_BOOST_CTRL1,
			0x38, (sel << 3));

	return 0;
}

static const char * const sma6201_trm_rmp_text[] = {
	"RMP_4.75A/us", "RMP_5.64A/us", "RMP_6.43A/us", "RMP_7.37A/us",
	"RMP_8.29A/us", "RMP_9.22A/us", "RMP_10.12A/us", "RMP_11.00A/us"};

static const struct soc_enum sma6201_trm_rmp_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_trm_rmp_text), sma6201_trm_rmp_text);

static int sma6201_trm_rmp_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_94_BOOST_CTRL1, &val);
	ucontrol->value.integer.value[0] = (val & 0x07);

	return 0;
}

static int sma6201_trm_rmp_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_94_BOOST_CTRL1,
			0x07, sel);

	return 0;
}

/* Boost control2 Set*/
static const char * const sma6201_trm_ocl_text[] = {
	"I_1.2A", "I_1.6A", "I_2.1A", "I_2.6A",
	"I_3.1A", "I_3.5A", "I_3.9A", "I_4.2A"};

static const struct soc_enum sma6201_trm_ocl_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_trm_ocl_text), sma6201_trm_ocl_text);

static int sma6201_trm_ocl_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_95_BOOST_CTRL2, &val);
	ucontrol->value.integer.value[0] = ((val & 0x70) >> 4);

	return 0;
}

static int sma6201_trm_ocl_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_95_BOOST_CTRL2,
			0x70, (sel << 4));

	return 0;
}

static const char * const sma6201_trm_comp_text[] = {
	"COMP_4.5Mohm/0.7pF", "COMP_4.0Mohm/2.0pF", "COMP_3.5Mohm/0.7pF",
	"COMP_3.0Mohm/2.0pF", "COMP_2.5Mohm/0.7pF", "COMP_2.0Mohm/2.0pF",
	"COMP_1.5Mohm/0.7pF", "COMP_1.0Mohm/2.0pF", "COMP_4.5Mohm/0.7pF",
	"COMP_4.0Mohm/2.0pF", "COMP_3.5Mohm/0.7pF", "COMP_3.0Mohm/2.0pF",
	"COMP_2.5Mohm/0.7pF", "COMP_2.0Mohm/2.0pF", "COMP_1.5Mohm/0.7pF",
	"COMP_1.0Mohm/2.0pF"};

static const struct soc_enum sma6201_trm_comp_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_trm_comp_text), sma6201_trm_comp_text);

static int sma6201_trm_comp_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_95_BOOST_CTRL2, &val);
	ucontrol->value.integer.value[0] = (val & 0x0F);

	return 0;
}

static int sma6201_trm_comp_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_95_BOOST_CTRL2,
			0x0F, sel);

	return 0;
}

/* Boost control3 Set*/
static const char * const sma6201_trm_dt_text[] = {
	"Time_24.0ns", "Time_18.0ns", "Time_12.1ns", "Time_10.4ns",
	"Time_7.99ns", "Time_7.26ns", "Time_6.14ns", "Time_5.72ns",
	"Time_4.00ns", "Time_3.83ns", "Time_3.54ns", "Time_3.42ns",
	"Time_1.97ns", "Time_1.95ns", "Time_1.90ns", "Time_1.88ns"};

static const struct soc_enum sma6201_trm_dt_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_trm_dt_text), sma6201_trm_dt_text);

static int sma6201_trm_dt_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_96_BOOST_CTRL3, &val);
	ucontrol->value.integer.value[0] = ((val & 0xF0) >> 4);

	return 0;
}

static int sma6201_trm_dt_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_96_BOOST_CTRL3,
			0xF0, (sel << 4));

	return 0;
}

static const char * const sma6201_trm_slw_text[] = {
	"Time_6ns", "Time_4ns", "Time_3ns", "Time_2ns"};

static const struct soc_enum sma6201_trm_slw_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_trm_slw_text), sma6201_trm_slw_text);

static int sma6201_trm_slw_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_96_BOOST_CTRL3, &val);
	ucontrol->value.integer.value[0] = (val & 0x03);

	return 0;
}

static int sma6201_trm_slw_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_96_BOOST_CTRL3,
			0x03, sel);

	return 0;
}

/* Boost control4 Set*/
static const char * const sma6201_trm_vbst2_text[] = {
	"BST_0.60/0.40/0.28", "BST_0.60/0.40/0.30", "BST_0.60/0.40/0.32",
	"BST_0.60/0.40/0.35", "BST_0.60/0.45/0.28", "BST_0.60/0.45/0.30",
	"BST_0.60/0.45/0.32", "BST_0.60/0.45/0.35", "BST_0.60/0.50/0.28",
	"BST_0.60/0.50/0.30", "BST_0.60/0.50/0.32", "BST_0.60/0.50/0.35",
	"BST_0.60/0.55/0.28", "BST_0.60/0.55/0.30", "BST_0.60/0.55/0.32",
	"BST_0.60/0.55/0.35", "BST_0.65/0.40/0.28", "BST_0.65/0.40/0.30",
	"BST_0.65/0.40/0.32", "BST_0.65/0.40/0.35", "BST_0.65/0.45/0.28",
	"BST_0.65/0.45/0.30", "BST_0.65/0.45/0.32", "BST_0.65/0.45/0.35",
	"BST_0.65/0.50/0.28", "BST_0.65/0.50/0.30", "BST_0.65/0.50/0.32",
	"BST_0.65/0.50/0.35", "BST_0.65/0.55/0.28", "BST_0.65/0.55/0.30",
	"BST_0.65/0.55/0.32", "BST_0.65/0.55/0.35", "BST_0.70/0.40/0.28",
	"BST_0.70/0.40/0.30", "BST_0.70/0.40/0.32", "BST_0.70/0.40/0.35",
	"BST_0.70/0.45/0.28", "BST_0.70/0.45/0.30", "BST_0.70/0.45/0.32",
	"BST_0.70/0.45/0.35", "BST_0.70/0.50/0.28", "BST_0.70/0.50/0.30",
	"BST_0.70/0.50/0.32", "BST_0.70/0.50/0.35", "BST_0.70/0.55/0.28",
	"BST_0.70/0.55/0.30", "BST_0.70/0.55/0.32", "BST_0.70/0.55/0.35",
	"BST_0.75/0.40/0.28", "BST_0.75/0.40/0.30", "BST_0.75/0.40/0.32",
	"BST_0.75/0.40/0.35", "BST_0.75/0.45/0.28", "BST_0.75/0.45/0.30",
	"BST_0.75/0.45/0.32", "BST_0.75/0.45/0.35", "BST_0.75/0.50/0.28",
	"BST_0.75/0.50/0.30", "BST_0.75/0.50/0.32", "BST_0.75/0.50/0.35",
	"BST_0.75/0.55/0.28", "BST_0.75/0.55/0.30", "BST_0.75/0.55/0.32",
	"BST_0.75/0.55/0.35"};

static const struct soc_enum sma6201_trm_vbst2_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_trm_vbst2_text), sma6201_trm_vbst2_text);

static int sma6201_trm_vbst2_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_97_BOOST_CTRL4, &val);
	ucontrol->value.integer.value[0] = ((val & 0xFC) >> 2);

	return 0;
}

static int sma6201_trm_vbst2_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 63))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_97_BOOST_CTRL4,
			0xFC, (sel << 2));

	return 0;
}

static const char * const sma6201_trm_tmin_text[] = {
	"Time_59ns", "Time_68ns", "Time_77ns", "Time_86ns"};

static const struct soc_enum sma6201_trm_tmin_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_trm_tmin_text), sma6201_trm_tmin_text);

static int sma6201_trm_tmin_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_97_BOOST_CTRL4, &val);
	ucontrol->value.integer.value[0] = (val & 0x03);

	return 0;
}

static int sma6201_trm_tmin_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_97_BOOST_CTRL4,
			0x03, sel);

	return 0;
}

/* General Setting Set */
static const char * const sma6201_adc_sr_text[] = {
	"f_192kHz", "f_96kHz", "f_48kHz", "f_24kHz",
	"f_12kHz", "Reserved", "Reserved", "Reserved"};

static const struct soc_enum sma6201_adc_sr_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_adc_sr_text), sma6201_adc_sr_text);

static int sma6201_adc_sr_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_98_GENERAL_SETTING, &val);
	ucontrol->value.integer.value[0] = ((val & 0x1C) >> 2);

	return 0;
}

static int sma6201_adc_sr_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_98_GENERAL_SETTING,
			0x1C, (sel << 2));

	return 0;
}

/* Volume IADC Set */
static int adc_digital_vol_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_9A_VOLUME_IADC);
}

static int adc_digital_vol_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_9A_VOLUME_IADC);
}

/* Volume PGA ISENSE Set */
static const char * const sma6201_pgavol_i_text[] = {
	"X3", "X4", "X5", "X6", "X7", "X8", "X9", "X10"};

static const struct soc_enum sma6201_pgavol_i_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_pgavol_i_text), sma6201_pgavol_i_text);

static int sma6201_pgavol_i_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_9C_VOLUME_PGA_ISENSE, &val);
	ucontrol->value.integer.value[0] = ((val & 0x38) >> 3);

	return 0;
}

static int sma6201_pgavol_i_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_9C_VOLUME_PGA_ISENSE,
			0x38, (sel << 3));

	return 0;
}

static const char * const sma6201_ptat_res_ctrl_text[] = {
	"R_108ohm", "R_112ohm", "R_116ohm", "R_120ohm",
	"R_124ohm", "R_128ohm", "R_132ohm", "R_136ohm"};

static const struct soc_enum sma6201_ptat_res_ctrl_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_ptat_res_ctrl_text),
		sma6201_ptat_res_ctrl_text);

static int sma6201_ptat_res_ctrl_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_9C_VOLUME_PGA_ISENSE, &val);
	ucontrol->value.integer.value[0] = (val & 0x07);

	return 0;
}

static int sma6201_ptat_res_ctrl_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_9C_VOLUME_PGA_ISENSE,
			0x07, sel);

	return 0;
}

/* TRIM ISENSE Current 1_2 Set */
static int trim_isense_current_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_9E_TRIM_ISENSE_CUR1);
}

static int trim_isense_current_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_9E_TRIM_ISENSE_CUR1);
}

/* SYSCLK / Volume and Mute Slope Set */
static const char * const sma6201_adc_sys_clk_text[] = {
	"fs_128", "fs_256", "fs_512", "fs_1024"};

static const struct soc_enum sma6201_adc_sys_clk_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_adc_sys_clk_text),
		sma6201_adc_sys_clk_text);

static int sma6201_adc_sys_clk_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_A0_ADC_MUTE_VOL_CTRL, &val);
	ucontrol->value.integer.value[0] = ((val & 0xC0) >> 6);

	return 0;
}

static int sma6201_adc_sys_clk_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_A0_ADC_MUTE_VOL_CTRL,
			0xC0, (sel << 6));

	return 0;
}

static const char * const sma6201_adc_mute_slope_text[] = {
	"Direct change", "dB per 32 sample",
	"dB per 64 sample", "dB per 96 sample"};

static const struct soc_enum sma6201_adc_mute_slope_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_adc_mute_slope_text),
		sma6201_adc_mute_slope_text);

static int sma6201_adc_mute_slope_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_A0_ADC_MUTE_VOL_CTRL, &val);
	ucontrol->value.integer.value[0] = ((val & 0x18) >> 3);

	return 0;
}

static int sma6201_adc_mute_slope_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_A0_ADC_MUTE_VOL_CTRL,
			0x18, (sel << 3));

	return 0;
}

/* SDO OUTPUT FORMAT Set */
static const char * const sma6201_o_format_text[] = {
	"Reserved", "LJ", "I2S", "Reserved", "TDM",
	"Reserved", "Reserved", "Reserved"};

static const struct soc_enum sma6201_o_format_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_o_format_text), sma6201_o_format_text);

static int sma6201_o_format_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_A4_SDO_OUT_FMT, &val);
	ucontrol->value.integer.value[0] = ((val & 0xE0) >> 5);

	return 0;
}

static int sma6201_o_format_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_A4_SDO_OUT_FMT,
			0xE0, (sel << 5));

	return 0;
}

static const char * const sma6201_sck_rate_text[] = {
	"fs_64", "fs_64", "fs_32", "fs_32"};

static const struct soc_enum sma6201_sck_rate_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_sck_rate_text), sma6201_sck_rate_text);

static int sma6201_sck_rate_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_A4_SDO_OUT_FMT, &val);
	ucontrol->value.integer.value[0] = ((val & 0x18) >> 3);

	return 0;
}

static int sma6201_sck_rate_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_A4_SDO_OUT_FMT,
			0x18, (sel << 3));

	return 0;
}

static const char * const sma6201_wd_length_text[] = {
	"WD_24bit", "WD_20bit", "WD_16bit", "WD_16bit"};

static const struct soc_enum sma6201_wd_length_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_wd_length_text), sma6201_wd_length_text);

static int sma6201_wd_length_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_A4_SDO_OUT_FMT, &val);
	ucontrol->value.integer.value[0] = ((val & 0x06) >> 1);

	return 0;
}

static int sma6201_wd_length_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_A4_SDO_OUT_FMT,
			0x06, (sel << 1));

	return 0;
}

/* TDM1 Set */
static const char * const sma6201_tdm_slot1_rx_text[] = {
	"Slot_1", "Slot_2", "Slot_3", "Slot_4",
	"Slot_5", "Slot_6", "Slot_7"};

static const struct soc_enum sma6201_tdm_slot1_rx_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_tdm_slot1_rx_text),
		sma6201_tdm_slot1_rx_text);

static int sma6201_tdm_slot1_rx_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_A5_TDM1, &val);
	ucontrol->value.integer.value[0] = ((val & 0x38) >> 3);

	return 0;
}

static int sma6201_tdm_slot1_rx_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_A5_TDM1,
			0x38, (sel << 3));

	return 0;
}

static const char * const sma6201_tdm_slot2_rx_text[] = {
	"Slot_1", "Slot_2", "Slot_3", "Slot_4",
	"Slot_5", "Slot_6", "Slot_7"};

static const struct soc_enum sma6201_tdm_slot2_rx_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_tdm_slot2_rx_text),
		sma6201_tdm_slot2_rx_text);

static int sma6201_tdm_slot2_rx_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_A5_TDM1, &val);
	ucontrol->value.integer.value[0] = (val & 0x07);

	return 0;
}

static int sma6201_tdm_slot2_rx_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_A5_TDM1,
			0x07, sel);

	return 0;
}

/* TDM2 Set */
static const char * const sma6201_tdm_slot1_tx_text[] = {
	"Slot_1", "Slot_2", "Slot_3", "Slot_4",
	"Slot_5", "Slot_6", "Slot_7"};

static const struct soc_enum sma6201_tdm_slot1_tx_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_tdm_slot1_tx_text),
		sma6201_tdm_slot1_tx_text);

static int sma6201_tdm_slot1_tx_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_A6_TDM2, &val);
	ucontrol->value.integer.value[0] = ((val & 0x38) >> 3);

	return 0;
}

static int sma6201_tdm_slot1_tx_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_A6_TDM2,
			0x38, (sel << 3));

	return 0;
}

static const char * const sma6201_tdm_slot2_tx_text[] = {
	"Slot_1", "Slot_2", "Slot_3", "Slot_4",
	"Slot_5", "Slot_6", "Slot_7"};

static const struct soc_enum sma6201_tdm_slot2_tx_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_tdm_slot2_tx_text),
		sma6201_tdm_slot2_tx_text);

static int sma6201_tdm_slot2_tx_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_A6_TDM2, &val);
	ucontrol->value.integer.value[0] = (val & 0x07);

	return 0;
}

static int sma6201_tdm_slot2_tx_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_A6_TDM2,
			0x07, sel);

	return 0;
}

/* TOP_MAN3 Set */
static const char * const sma6201_test_clock_mon_time_sel_text[] = {
	"Time_80us", "Time_40us", "Time_20us", "Time_10us"};

static const struct soc_enum sma6201_test_clock_mon_time_sel_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_test_clock_mon_time_sel_text),
		sma6201_test_clock_mon_time_sel_text);

static int sma6201_test_clock_mon_time_sel_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_A7_TOP_MAN3, &val);
	ucontrol->value.integer.value[0] = ((val & 0xC0) >> 6);

	return 0;
}

static int sma6201_test_clock_mon_time_sel_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_A7_TOP_MAN3,
			0xC0, (sel << 6));

	return 0;
}

/* TONE_GENERATOR Set */
static const char * const sma6201_tone_freq_text[] = {
	"f_50Hz", "f_60Hz", "f_140Hz", "f_150Hz", "f_175Hz", "f_180Hz",
	"f_200Hz", "f_375Hz", "f_750Hz", "f_1kHz", "f_3kHz", "f_6kHz",
	"f_11.75kHz", "f_15kHz", "f_17kHz", "f_19kHz"};

static const struct soc_enum sma6201_tone_freq_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_tone_freq_text), sma6201_tone_freq_text);

static int sma6201_tone_freq_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_A8_TONE_GENERATOR, &val);
	ucontrol->value.integer.value[0] = ((val & 0x1E) >> 1);

	return 0;
}

static int sma6201_tone_freq_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 15))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_A8_TONE_GENERATOR,
			0x1E, (sel << 1));

	return 0;
}

/* TONE_FINE VOLUME Set */
static int tone_fine_volume_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_A9_TONE_FINE_VOL);
}

static int tone_fine_volume_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_A9_TONE_FINE_VOL);
}

/* PLL_A_D_Setting Set */
static int pll_a_d_setting_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_AA_PLL_A_SETTING);
}

static int pll_a_d_setting_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_AA_PLL_A_SETTING);
}

/* PLL LDO Control Set */
static int pll_ldo_ctrl_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_AC_PLL_CTRL);
}

static int pll_ldo_ctrl_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_AC_PLL_CTRL);
}

/* SPK_OCP LVL Set */
static const char * const sma6201_pwm_freq_text[] = {
	"f_680kHz", "f_640kHz", "f_620kHz", "f_600kHz",
	"f_740kHz", "f_640kHz", "f_620kHz", "f_600kHz"};

static const struct soc_enum sma6201_pwm_freq_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_pwm_freq_text),
		sma6201_pwm_freq_text);

static int sma6201_pwm_freq_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_AD_SPK_OCP_LVL, &val);
	ucontrol->value.integer.value[0] = ((val & 0x70) >> 4);

	return 0;
}

static int sma6201_pwm_freq_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 7))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_AD_SPK_OCP_LVL,
			0x70, (sel << 4));

	return 0;
}

static const char * const sma6201_ocp_filter_text[] = {
	"Filter_0(Slowest)", "Filter_1", "Filter_2", "Filter_3(Fastest)"};

static const struct soc_enum sma6201_ocp_filter_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_ocp_filter_text),
		sma6201_ocp_filter_text);

static int sma6201_ocp_filter_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_AD_SPK_OCP_LVL, &val);
	ucontrol->value.integer.value[0] = ((val & 0x0C) >> 2);

	return 0;
}

static int sma6201_ocp_filter_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_AD_SPK_OCP_LVL,
			0x0C, (sel << 2));

	return 0;
}

static const char * const sma6201_ocp_lvl_text[] = {
	"I_2.6A", "I_3.1A", "I_3.7A", "I_4.2A"};

static const struct soc_enum sma6201_ocp_lvl_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_ocp_lvl_text), sma6201_ocp_lvl_text);

static int sma6201_ocp_lvl_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_AD_SPK_OCP_LVL, &val);
	ucontrol->value.integer.value[0] = (val & 0x03);

	return 0;
}

static int sma6201_ocp_lvl_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_AD_SPK_OCP_LVL, 0x03, sel);

	return 0;
}

/* TOP_MAN4 Set */
static const char * const sma6201_sdo_data_select_text[] = {
	"DAC/DAC", "DAC/ADC", "DAC/ADC_24", "ADC/DAC_24"};

static const struct soc_enum sma6201_sdo_data_select_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6201_sdo_data_select_text),
		sma6201_sdo_data_select_text);

static int sma6201_sdo_data_select_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int val;

	regmap_read(sma6201->regmap, SMA6201_AE_TOP_MAN4, &val);
	ucontrol->value.integer.value[0] = ((val & 0x30) >> 4);

	return 0;
}

static int sma6201_sdo_data_select_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int sel = (int)ucontrol->value.integer.value[0];

	if ((sel < 0) || (sel > 3))
		return -EINVAL;

	regmap_update_bits(sma6201->regmap, SMA6201_AE_TOP_MAN4,
			0x30, (sel << 4));

	return 0;
}

/* VIN Sensing Set */
static int vin_sensing_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_AF_VIN_SENSING);
}

static int vin_sensing_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_AF_VIN_SENSING);
}

/* Brown Out Protection 0-15 Set */
static int brown_out_pt_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_get(kcontrol, ucontrol, SMA6201_B0_BROWN_OUT_P0);
}

static int brown_out_pt_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	return bytes_ext_put(kcontrol, ucontrol, SMA6201_B0_BROWN_OUT_P0);
}

static const char * const voice_music_class_h_mode_text[] = {
	"Voice", "Music", "Off"};

static const struct soc_enum voice_music_class_h_mode_enum =
SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(voice_music_class_h_mode_text),
	voice_music_class_h_mode_text);

static int voice_music_class_h_mode_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = sma6201->voice_music_class_h_mode;

	return 0;
}

static int voice_music_class_h_mode_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	sma6201->voice_music_class_h_mode = ucontrol->value.integer.value[0];

	if ((sma6201->voice_music_class_h_mode < 0) ||
	(sma6201->voice_music_class_h_mode > 2))
		return -EINVAL;

	switch (sma6201->voice_music_class_h_mode) {
	case SMA6201_CLASS_H_VOICE_MODE:
	/* FDPEC gain & Boost voltage in
	 * voice scenario
	 */
	if (sma6201->rev_num == REV_NUM_REV0) {
		dev_info(component->dev, "%s : FDPEC gain 3 & Boost 8V in voice scenario\n",
				__func__);
		regmap_write(sma6201->regmap,
			SMA6201_0D_CLASS_H_CTRL_LVL1, 0xFC);
		regmap_write(sma6201->regmap,
			SMA6201_0F_CLASS_H_CTRL_LVL2, 0xFB);
		regmap_update_bits(sma6201->regmap,
			SMA6201_93_BOOST_CTRL0,
				TRM_VBST1_MASK, TRM_VBST1_8V);
		regmap_update_bits(sma6201->regmap,
			SMA6201_95_BOOST_CTRL2,
				TRM_OCL_MASK, TRM_OCL_1P6_A);
		regmap_write(sma6201->regmap,
			SMA6201_97_BOOST_CTRL4, 0xA4);
		regmap_write(sma6201->regmap,
			SMA6201_A9_TONE_FINE_VOL, 0xA7);
		regmap_update_bits(sma6201->regmap,
			SMA6201_13_FDPEC_CTRL1,
				FDPEC_GAIN_MASK, FDPEC_GAIN_3);
		regmap_write(sma6201->regmap,
			SMA6201_91_CLASS_H_CTRL_LVL7, 0xB6);
	} else {
		dev_info(component->dev, "%s : FDPEC gain 2 & Boost 8V in voice scenario\n",
				__func__);
		/* HPF Frequency - 82 Hz */
		regmap_write(sma6201->regmap,
			SMA6201_15_BASS_SPK1, 0x02);
		regmap_write(sma6201->regmap,
			SMA6201_16_BASS_SPK2, 0x08);
		regmap_write(sma6201->regmap,
			SMA6201_17_BASS_SPK3, 0x08);
		regmap_write(sma6201->regmap,
			SMA6201_18_BASS_SPK4, 0x11);
		regmap_write(sma6201->regmap,
			SMA6201_19_BASS_SPK5, 0x6E);
		regmap_write(sma6201->regmap,
			SMA6201_1A_BASS_SPK6, 0x33);
		regmap_write(sma6201->regmap,
			SMA6201_1B_BASS_SPK7, 0x0A);

		regmap_write(sma6201->regmap,
			SMA6201_0D_CLASS_H_CTRL_LVL1, 0xF7);
		regmap_write(sma6201->regmap,
			SMA6201_0F_CLASS_H_CTRL_LVL2, 0xF6);
		regmap_write(sma6201->regmap,
			SMA6201_28_CLASS_H_CTRL_LVL3, 0x15);
		regmap_write(sma6201->regmap,
			SMA6201_29_CLASS_H_CTRL_LVL4, 0x24);
		regmap_write(sma6201->regmap,
			SMA6201_2A_CLASS_H_CTRL_LVL5, 0x23);
		regmap_write(sma6201->regmap,
			SMA6201_90_CLASS_H_CTRL_LVL6, 0x52);
		regmap_write(sma6201->regmap,
			SMA6201_91_CLASS_H_CTRL_LVL7, 0xB1);
		regmap_write(sma6201->regmap,
			SMA6201_38_DIS_CLASSH_LVL12, 0xCC);

		regmap_write(sma6201->regmap,
			SMA6201_95_BOOST_CTRL2, 0x0E);
		regmap_write(sma6201->regmap,
			SMA6201_97_BOOST_CTRL4, 0xE9);
		regmap_write(sma6201->regmap,
			SMA6201_A9_TONE_FINE_VOL, 0x87);
		regmap_write(sma6201->regmap,
			SMA6201_0A_SPK_VOL, 0x30);

		regmap_write(sma6201->regmap,
			SMA6201_23_COMP_LIM1, 0x1F);
		regmap_write(sma6201->regmap,
			SMA6201_24_COMP_LIM2, 0x02);
		regmap_write(sma6201->regmap,
			SMA6201_25_COMP_LIM3, 0x09);
		regmap_write(sma6201->regmap,
			SMA6201_26_COMP_LIM4, 0xFF);

		regmap_update_bits(sma6201->regmap,
			SMA6201_13_FDPEC_CTRL1,
				FDPEC_GAIN_MASK, FDPEC_GAIN_2);
		regmap_update_bits(sma6201->regmap,
			SMA6201_93_BOOST_CTRL0,
				TRM_VBST1_MASK, TRM_VBST1_8V);
		regmap_update_bits(sma6201->regmap,
			SMA6201_95_BOOST_CTRL2,
				TRM_OCL_MASK, TRM_OCL_1P2_A);
	}
	regmap_update_bits(sma6201->regmap,
		SMA6201_92_FDPEC_CTRL2,
			EN_DGC_MASK, DGC_DISABLE);
	break;

	case SMA6201_CLASS_H_MUSIC_MODE:
	/* FDPEC gain & Boost voltage in
	 * music scenario
	 */
	dev_info(component->dev, "%s : FDPEC gain 8 & Boost 18V in music scenario\n",
			__func__);
	if (sma6201->rev_num == REV_NUM_REV0) {
		regmap_write(sma6201->regmap,
			SMA6201_0D_CLASS_H_CTRL_LVL1, 0x4C);
		regmap_write(sma6201->regmap,
			SMA6201_0F_CLASS_H_CTRL_LVL2, 0x3B);
		regmap_write(sma6201->regmap,
			SMA6201_91_CLASS_H_CTRL_LVL7, 0xD6);
		regmap_write(sma6201->regmap,
			SMA6201_97_BOOST_CTRL4, 0xE4);
		regmap_write(sma6201->regmap,
			SMA6201_A9_TONE_FINE_VOL, 0x87);
		regmap_update_bits(sma6201->regmap,
			SMA6201_93_BOOST_CTRL0,
				TRM_VBST1_MASK, TRM_VBST1_18V);
		regmap_update_bits(sma6201->regmap,
			SMA6201_92_FDPEC_CTRL2,
				EN_DGC_MASK, DGC_ENABLE);
	} else {
		/* HPF Frequency - 201 Hz */
		regmap_write(sma6201->regmap,
			SMA6201_15_BASS_SPK1, 0x06);
		regmap_write(sma6201->regmap,
			SMA6201_16_BASS_SPK2, 0x05);
		regmap_write(sma6201->regmap,
			SMA6201_17_BASS_SPK3, 0x05);
		regmap_write(sma6201->regmap,
			SMA6201_18_BASS_SPK4, 0x0E);
		regmap_write(sma6201->regmap,
			SMA6201_19_BASS_SPK5, 0x61);
		regmap_write(sma6201->regmap,
			SMA6201_1A_BASS_SPK6, 0x0B);
		regmap_write(sma6201->regmap,
			SMA6201_1B_BASS_SPK7, 0x06);

		regmap_write(sma6201->regmap,
			SMA6201_0D_CLASS_H_CTRL_LVL1, 0x9C);
		regmap_write(sma6201->regmap,
			SMA6201_0F_CLASS_H_CTRL_LVL2, 0x6B);
		regmap_write(sma6201->regmap,
			SMA6201_28_CLASS_H_CTRL_LVL3, 0x7A);
		regmap_write(sma6201->regmap,
			SMA6201_29_CLASS_H_CTRL_LVL4, 0xA9);
		regmap_write(sma6201->regmap,
			SMA6201_2A_CLASS_H_CTRL_LVL5, 0x68);
		regmap_write(sma6201->regmap,
			SMA6201_90_CLASS_H_CTRL_LVL6, 0x97);
		regmap_write(sma6201->regmap,
			SMA6201_91_CLASS_H_CTRL_LVL7, 0xC6);
		regmap_write(sma6201->regmap,
			SMA6201_38_DIS_CLASSH_LVL12, 0xC8);

		regmap_write(sma6201->regmap,
			SMA6201_95_BOOST_CTRL2, 0x4E);
		regmap_write(sma6201->regmap,
			SMA6201_97_BOOST_CTRL4, 0x41);
		regmap_write(sma6201->regmap,
			SMA6201_A9_TONE_FINE_VOL, 0x77);
		regmap_write(sma6201->regmap,
			SMA6201_0A_SPK_VOL, 0x32);

		regmap_write(sma6201->regmap,
			SMA6201_23_COMP_LIM1, 0x1E);
		regmap_write(sma6201->regmap,
			SMA6201_24_COMP_LIM2, 0x2E);
		regmap_write(sma6201->regmap,
			SMA6201_25_COMP_LIM3, 0x09);
		regmap_write(sma6201->regmap,
			SMA6201_26_COMP_LIM4, 0xFF);

		regmap_update_bits(sma6201->regmap,
			SMA6201_93_BOOST_CTRL0,
				TRM_VBST1_MASK, TRM_VBST1_18V);
		regmap_update_bits(sma6201->regmap,
			SMA6201_92_FDPEC_CTRL2,
				EN_DGC_MASK, DGC_DISABLE);
	}
	regmap_update_bits(sma6201->regmap,
		SMA6201_13_FDPEC_CTRL1,
			FDPEC_GAIN_MASK, FDPEC_GAIN_8);

	regmap_update_bits(sma6201->regmap,
		SMA6201_95_BOOST_CTRL2,
			TRM_OCL_MASK, TRM_OCL_3P1_A);

	break;

	default:
	/* FDPEC gain & Boost voltage in
	 * music scenario
	 */
	dev_info(component->dev, "%s : FDPEC gain 8 & Boost 18V in music scenario\n",
			__func__);
	if (sma6201->rev_num == REV_NUM_REV0) {
		regmap_write(sma6201->regmap,
			SMA6201_0D_CLASS_H_CTRL_LVL1, 0x4C);
		regmap_write(sma6201->regmap,
			SMA6201_0F_CLASS_H_CTRL_LVL2, 0x3B);
		regmap_write(sma6201->regmap,
			SMA6201_91_CLASS_H_CTRL_LVL7, 0xD6);
		regmap_write(sma6201->regmap,
			SMA6201_97_BOOST_CTRL4, 0xE4);
		regmap_write(sma6201->regmap,
			SMA6201_A9_TONE_FINE_VOL, 0x87);
		regmap_update_bits(sma6201->regmap,
			SMA6201_93_BOOST_CTRL0,
				TRM_VBST1_MASK, TRM_VBST1_18V);
		regmap_update_bits(sma6201->regmap,
			SMA6201_92_FDPEC_CTRL2,
				EN_DGC_MASK, DGC_ENABLE);
	} else {
		/* HPF Frequency - 201 Hz */
		regmap_write(sma6201->regmap,
			SMA6201_15_BASS_SPK1, 0x06);
		regmap_write(sma6201->regmap,
			SMA6201_16_BASS_SPK2, 0x05);
		regmap_write(sma6201->regmap,
			SMA6201_17_BASS_SPK3, 0x05);
		regmap_write(sma6201->regmap,
			SMA6201_18_BASS_SPK4, 0x0E);
		regmap_write(sma6201->regmap,
			SMA6201_19_BASS_SPK5, 0x61);
		regmap_write(sma6201->regmap,
			SMA6201_1A_BASS_SPK6, 0x0B);
		regmap_write(sma6201->regmap,
			SMA6201_1B_BASS_SPK7, 0x06);

		regmap_write(sma6201->regmap,
			SMA6201_0D_CLASS_H_CTRL_LVL1, 0x9C);
		regmap_write(sma6201->regmap,
			SMA6201_0F_CLASS_H_CTRL_LVL2, 0x6B);
		regmap_write(sma6201->regmap,
			SMA6201_28_CLASS_H_CTRL_LVL3, 0x7A);
		regmap_write(sma6201->regmap,
			SMA6201_29_CLASS_H_CTRL_LVL4, 0xA9);
		regmap_write(sma6201->regmap,
			SMA6201_2A_CLASS_H_CTRL_LVL5, 0x68);
		regmap_write(sma6201->regmap,
			SMA6201_90_CLASS_H_CTRL_LVL6, 0x97);
		regmap_write(sma6201->regmap,
			SMA6201_91_CLASS_H_CTRL_LVL7, 0xC6);
		regmap_write(sma6201->regmap,
			SMA6201_38_DIS_CLASSH_LVL12, 0xC8);

		regmap_write(sma6201->regmap,
			SMA6201_95_BOOST_CTRL2, 0x4E);
		regmap_write(sma6201->regmap,
			SMA6201_97_BOOST_CTRL4, 0x41);
		regmap_write(sma6201->regmap,
			SMA6201_A9_TONE_FINE_VOL, 0x77);
		regmap_write(sma6201->regmap,
			SMA6201_0A_SPK_VOL, 0x32);

		regmap_write(sma6201->regmap,
			SMA6201_23_COMP_LIM1, 0x1E);
		regmap_write(sma6201->regmap,
			SMA6201_24_COMP_LIM2, 0x2E);
		regmap_write(sma6201->regmap,
			SMA6201_25_COMP_LIM3, 0x09);
		regmap_write(sma6201->regmap,
			SMA6201_26_COMP_LIM4, 0xFF);

		regmap_update_bits(sma6201->regmap,
			SMA6201_93_BOOST_CTRL0,
				TRM_VBST1_MASK, TRM_VBST1_18V);
		regmap_update_bits(sma6201->regmap,
			SMA6201_92_FDPEC_CTRL2,
				EN_DGC_MASK, DGC_DISABLE);
	}
	regmap_update_bits(sma6201->regmap,
		SMA6201_13_FDPEC_CTRL1,
			FDPEC_GAIN_MASK, FDPEC_GAIN_8);

	regmap_update_bits(sma6201->regmap,
		SMA6201_95_BOOST_CTRL2,
			TRM_OCL_MASK, TRM_OCL_3P1_A);
	}

	return 0;
}

static int sma6201_put_volsw(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component =
		snd_soc_kcontrol_component(kcontrol);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	unsigned int reg = mc->reg;
	int ret;
	unsigned int val;

	mutex_lock(&sma6201->lock);

	ret = snd_soc_put_volsw(kcontrol, ucontrol);
	if (ret < 0) {
		mutex_unlock(&sma6201->lock);
		return ret;
	}
	regmap_read(sma6201->regmap, reg, &val);

	if (val != sma6201->init_vol) {
		dev_dbg(component->dev, "%s :  init vol[%d] updated to vol[%d]\n",
		__func__, sma6201->init_vol, val);

		sma6201->init_vol = val;
	}
	mutex_unlock(&sma6201->lock);

	return 0;
}

static const struct snd_kcontrol_new sma6201_snd_controls[] = {

SOC_SINGLE_EXT("Power Up(1:up_0:down)", SND_SOC_NOPM, 0, 1, 0,
	power_up_down_control_get, power_up_down_control_put),
SOC_SINGLE_EXT("Force AMP Power Down", SND_SOC_NOPM, 0, 1, 0,
	power_down_control_get, power_down_control_put),
SOC_ENUM_EXT("External Clock System", sma6201_clk_system_enum,
	sma6201_clk_system_get, sma6201_clk_system_put),

SOC_SINGLE("I2S Clock mode(1:mst_0:slv)",
		SMA6201_01_INPUT1_CTRL1, 7, 1, 0),
SOC_ENUM_EXT("I2S input fmt(I2S_LJ_RJ)", sma6201_input_format_enum,
	sma6201_input_format_get, sma6201_input_format_put),
SOC_SINGLE("Fst-ch pol I2S(1:high_0:low)",
		SMA6201_01_INPUT1_CTRL1, 3, 1, 0),
SOC_SINGLE("Data SCK edge(1:rise_0:fall)",
		SMA6201_01_INPUT1_CTRL1, 2, 1, 0),

SOC_SINGLE("Add tone vol(1:decre_0:nml)",
		SMA6201_03_INPUT1_CTRL3, 5, 1, 0),
SOC_SINGLE("SRC(1:bypass_0:nml)",
		SMA6201_03_INPUT1_CTRL3, 4, 1, 0),

SND_SOC_BYTES_EXT("Piezo Filter Tune", 1,
	piezo_filter_tune_get, piezo_filter_tune_put),

SND_SOC_BYTES_EXT("BrownOut Set 1_4", 4,
	brown_out_set1_get, brown_out_set1_put),

SOC_ENUM_EXT("Port In/Out port config", sma6201_port_config_enum,
	sma6201_port_config_get, sma6201_port_config_put),
SOC_ENUM_EXT("Port Output Format", sma6201_port_out_format_enum,
	sma6201_port_out_format_get, sma6201_port_out_format_put),
SOC_ENUM_EXT("Port Output Source", sma6201_port_out_sel_enum,
	sma6201_port_out_sel_get, sma6201_port_out_sel_put),

SOC_SINGLE_EXT_TLV("SPK Volume", SMA6201_0A_SPK_VOL,
	0, 0xA8, 0, snd_soc_get_volsw, sma6201_put_volsw, sma6201_spk_tlv),

SND_SOC_BYTES_EXT("BrownOut Set 5_6", 2,
	brown_out_set2_get, brown_out_set2_put),

SOC_ENUM_EXT("Attack level control1", sma6201_attack_lvl_1_enum,
	sma6201_attack_lvl_1_get, sma6201_attack_lvl_1_put),
SOC_ENUM_EXT("Release time control1", sma6201_release_time_1_enum,
	sma6201_release_time_1_get, sma6201_release_time_1_put),

SOC_ENUM_EXT("Volume slope", sma6201_vol_slope_enum,
	sma6201_vol_slope_get, sma6201_vol_slope_put),
SOC_ENUM_EXT("DAC Mute slope", sma6201_mute_slope_enum,
	sma6201_mute_slope_get, sma6201_mute_slope_put),
SOC_SINGLE("SPK Mute Switch(1:mute_0:un)",
		SMA6201_0E_MUTE_VOL_CTRL, 0, 1, 0),

SOC_ENUM_EXT("Attack level control2", sma6201_attack_lvl_2_enum,
	sma6201_attack_lvl_2_get, sma6201_attack_lvl_2_put),
SOC_ENUM_EXT("Release time control2", sma6201_release_time_2_enum,
	sma6201_release_time_2_get, sma6201_release_time_2_put),

SOC_ENUM_EXT("SPK Mode", sma6201_spkmode_enum,
	sma6201_spkmode_get, sma6201_spkmode_put),

SOC_SINGLE("EQ(1:on_0:off)", SMA6201_11_SYSTEM_CTRL2, 7, 1, 0),
SOC_SINGLE("Bass(1:on_0:off)", SMA6201_11_SYSTEM_CTRL2, 6, 1, 0),
SOC_SINGLE("Comp/Limiter(1:on_0:off)",
		SMA6201_11_SYSTEM_CTRL2, 5, 1, 0),
SOC_SINGLE("LR Data(1:swap_0:nml)", SMA6201_11_SYSTEM_CTRL2, 4, 1, 0),
SOC_SINGLE("Mono Mix(1:on_0:off)", SMA6201_11_SYSTEM_CTRL2, 0, 1, 0),

SOC_ENUM_EXT("Input gain", sma6201_input_gain_enum,
	sma6201_input_gain_get, sma6201_input_gain_put),
SOC_ENUM_EXT("Input gain right channel", sma6201_input_r_gain_enum,
	sma6201_input_r_gain_get, sma6201_input_r_gain_put),

SOC_SINGLE("Dis ClassH2(1:dis_0:en)", SMA6201_13_FDPEC_CTRL1, 7, 1, 0),
SOC_SINGLE("Dis ClassH1(1:dis_0:en)", SMA6201_13_FDPEC_CTRL1, 6, 1, 0),
SOC_SINGLE("SDM Sync(1:off_0:on)", SMA6201_13_FDPEC_CTRL1, 5, 1, 0),
SOC_ENUM_EXT("HDC OPAMP I", sma6201_fdpec_i_enum,
	sma6201_fdpec_i_get, sma6201_fdpec_i_put),
SOC_ENUM_EXT("FDPEC Gain",
	fdpec_gain_control_enum,
	fdpec_gain_control_get, fdpec_gain_control_put),

SOC_ENUM_EXT("Speaker HYSFB", sma6201_spk_hysfb_enum,
	sma6201_spk_hysfb_get, sma6201_spk_hysfb_put),
SND_SOC_BYTES_EXT("Speaker BDELAY", 1, spk_bdelay_get, spk_bdelay_put),

SND_SOC_BYTES_EXT("Bass Boost SPK Coeff", 7,
	bass_spk_coeff_get, bass_spk_coeff_put),

SND_SOC_BYTES_EXT("Brown Out Protection 16_20", 5,
	brown_out_pt2_get, brown_out_pt2_put),

SND_SOC_BYTES_EXT("DGC Delay Set", 1,
	dgc_delay_get, dgc_delay_put),

SND_SOC_BYTES_EXT("Prescaler Set", 1,
	prescaler_get, prescaler_put),

SND_SOC_BYTES_EXT("DRC SPK Coeff", 4,
	comp_lim_spk_coeff_get, comp_lim_spk_coeff_put),

SOC_ENUM_EXT("Attack level control3", sma6201_attack_lvl_3_enum,
	sma6201_attack_lvl_3_get, sma6201_attack_lvl_3_put),
SOC_ENUM_EXT("Release time control3", sma6201_release_time_3_enum,
	sma6201_release_time_3_get, sma6201_release_time_3_put),
SOC_ENUM_EXT("Attack level control4", sma6201_attack_lvl_4_enum,
	sma6201_attack_lvl_4_get, sma6201_attack_lvl_4_put),
SOC_ENUM_EXT("Release time control4", sma6201_release_time_4_enum,
	sma6201_release_time_4_get, sma6201_release_time_4_put),
SOC_ENUM_EXT("Attack level control5", sma6201_attack_lvl_5_enum,
	sma6201_attack_lvl_5_get, sma6201_attack_lvl_5_put),
SOC_ENUM_EXT("Release time control5", sma6201_release_time_5_enum,
	sma6201_release_time_5_get, sma6201_release_time_5_put),

SOC_SINGLE("EQ output(1:EQ1pEQ2_0:EQ1)", SMA6201_2B_EQ_MODE, 4, 1, 0),
SOC_SINGLE("EQ bank sel(1:EQ2_0:EQ1)", SMA6201_2B_EQ_MODE, 3, 1, 0),

SOC_SINGLE("EQ2 band1(1:bp_0:op)", SMA6201_2C_EQBAND1_BYP, 6, 1, 0),
SOC_SINGLE("EQ1 band1(1:bp_0:op)", SMA6201_2C_EQBAND1_BYP, 5, 1, 0),
SOC_SINGLE("EQ2 band2(1:bp_0:op)", SMA6201_2D_EQBAND2_BYP, 6, 1, 0),
SOC_SINGLE("EQ1 band2(1:bp_0:op)", SMA6201_2D_EQBAND2_BYP, 5, 1, 0),
SOC_SINGLE("EQ2 band3(1:bp_0:op)", SMA6201_2E_EQBAND3_BYP, 6, 1, 0),
SOC_SINGLE("EQ1 band3(1:bp_0:op)", SMA6201_2E_EQBAND3_BYP, 5, 1, 0),
SOC_SINGLE("EQ2 band4(1:bp_0:op)", SMA6201_2F_EQBAND4_BYP, 6, 1, 0),
SOC_SINGLE("EQ1 band4(1:bp_0:op)", SMA6201_2F_EQBAND4_BYP, 5, 1, 0),
SOC_SINGLE("EQ2 band5(1:bp_0:op)", SMA6201_30_EQBAND5_BYP, 6, 1, 0),
SOC_SINGLE("EQ1 band5(1:bp_0:op)", SMA6201_30_EQBAND5_BYP, 5, 1, 0),

SOC_SINGLE("SDM VLINK(1:off_0:on)",
		SMA6201_33_SDM_CTRL, 3, 1, 0),
SOC_SINGLE("SDM Q Select(1:1/8_0:1/4)", SMA6201_33_SDM_CTRL, 2, 1, 0),

SOC_SINGLE("Edge displace(1:off_0:on)",
		SMA6201_36_PROTECTION, 7, 1, 0),
SOC_SINGLE("SRC random jitter(1:off_0:add)",
		SMA6201_36_PROTECTION, 4, 1, 0),
SOC_SINGLE("OCP SPK output(1:off_0:on)",
		SMA6201_36_PROTECTION, 3, 1, 0),
SOC_SINGLE("OCP mode(1:PSD_0:auto recover)",
		SMA6201_36_PROTECTION, 2, 1, 0),
SOC_ENUM_EXT("OTP MODE", sma6201_otp_mode_enum,
		sma6201_otp_mode_get, sma6201_otp_mode_put),

SND_SOC_BYTES_EXT("SlopeCTRL", 1, slope_ctrl_get, slope_ctrl_put),

SND_SOC_BYTES_EXT("Disable class-H Level1_6", 3,
	dis_class_h_lvl_get, dis_class_h_lvl_put),

SND_SOC_BYTES_EXT("Test mode(Test_ATEST)", 5,
	test_mode_get, test_mode_put),

SND_SOC_BYTES_EXT("EQ1 Ctrl Band1", 15, eq1_ctrl_band1_get, eq1_ctrl_band1_put),
SND_SOC_BYTES_EXT("EQ1 Ctrl Band2", 15, eq1_ctrl_band2_get, eq1_ctrl_band2_put),
SND_SOC_BYTES_EXT("EQ1 Ctrl Band3", 15, eq1_ctrl_band3_get, eq1_ctrl_band3_put),
SND_SOC_BYTES_EXT("EQ1 Ctrl Band4", 15, eq1_ctrl_band4_get, eq1_ctrl_band4_put),
SND_SOC_BYTES_EXT("EQ1 Ctrl Band5", 15, eq1_ctrl_band5_get, eq1_ctrl_band5_put),

SND_SOC_BYTES_EXT("EQ2 Ctrl Band1", 15, eq2_ctrl_band1_get, eq2_ctrl_band1_put),
SND_SOC_BYTES_EXT("EQ2 Ctrl Band2", 15, eq2_ctrl_band2_get, eq2_ctrl_band2_put),
SND_SOC_BYTES_EXT("EQ2 Ctrl Band3", 15, eq2_ctrl_band3_get, eq2_ctrl_band3_put),
SND_SOC_BYTES_EXT("EQ2 Ctrl Band4", 15, eq2_ctrl_band4_get, eq2_ctrl_band4_put),
SND_SOC_BYTES_EXT("EQ2 Ctrl Band5", 15, eq2_ctrl_band5_get, eq2_ctrl_band5_put),

SND_SOC_BYTES_EXT("PLL Setting", 5, pll_setting_get, pll_setting_put),

SOC_ENUM_EXT("Attack level control6", sma6201_attack_lvl_6_enum,
	sma6201_attack_lvl_6_get, sma6201_attack_lvl_6_put),
SOC_ENUM_EXT("Release time control6", sma6201_release_time_6_enum,
	sma6201_release_time_6_get, sma6201_release_time_6_put),
SOC_ENUM_EXT("Attack level control7", sma6201_attack_lvl_7_enum,
	sma6201_attack_lvl_7_get, sma6201_attack_lvl_7_put),
SOC_ENUM_EXT("Release time control7", sma6201_release_time_7_enum,
	sma6201_release_time_7_get, sma6201_release_time_7_put),

SOC_ENUM_EXT("FDPEC Gain Trim", sma6201_fdpec_gain_trm_enum,
	sma6201_fdpec_gain_trm_get, sma6201_fdpec_gain_trm_put),
SOC_SINGLE("REC CUR Mode(1:N_0:E)",
		SMA6201_92_FDPEC_CTRL2, 5, 1, 0),
SOC_SINGLE("REC CUR Ctrl(1:off_0:on)",
		SMA6201_92_FDPEC_CTRL2, 4, 1, 0),
SOC_SINGLE("PWM frequency(1_0)",
		SMA6201_92_FDPEC_CTRL2, 3, 1, 0),
SOC_ENUM_EXT("OPAMP Bias I", sma6201_diffamp_i_enum,
	sma6201_diffamp_i_get, sma6201_diffamp_i_put),
SOC_SINGLE("DGC Control(1:on_0:off)",
		SMA6201_92_FDPEC_CTRL2, 0, 1, 0),

SOC_ENUM_EXT("Trim of VBG reference", sma6201_trm_vref_enum,
	sma6201_trm_vref_get, sma6201_trm_vref_put),
SOC_ENUM_EXT("Trim of bst output V", sma6201_trm_vbst1_enum,
	sma6201_trm_vbst1_get, sma6201_trm_vbst1_put),

SOC_ENUM_EXT("Trim I-gain bst V loop", sma6201_trm_comp2_enum,
	sma6201_trm_comp2_get, sma6201_trm_comp2_put),
SOC_ENUM_EXT("Trim of switch freq", sma6201_trm_osc_enum,
	sma6201_trm_osc_get, sma6201_trm_osc_put),
SOC_ENUM_EXT("Trim slope compensation", sma6201_trm_rmp_enum,
	sma6201_trm_rmp_get, sma6201_trm_rmp_put),

SOC_ENUM_EXT("Trim of over I limit", sma6201_trm_ocl_enum,
	sma6201_trm_ocl_get, sma6201_trm_ocl_put),
SOC_ENUM_EXT("Trim P-gain I-gain", sma6201_trm_comp_enum,
	sma6201_trm_comp_get, sma6201_trm_comp_put),

SOC_ENUM_EXT("Trim of driver deadtime", sma6201_trm_dt_enum,
	sma6201_trm_dt_get, sma6201_trm_dt_put),
SOC_SINGLE("Bst I limit(1:on_0:off)",
		SMA6201_96_BOOST_CTRL3, 3, 1, 0),
SOC_SINGLE("Bst OCP(1:on_0:off)",
		SMA6201_96_BOOST_CTRL3, 2, 1, 0),
SOC_ENUM_EXT("Trim of switch slew", sma6201_trm_slw_enum,
	sma6201_trm_slw_get, sma6201_trm_slw_put),

SOC_ENUM_EXT("Trim of bst reference", sma6201_trm_vbst2_enum,
	sma6201_trm_vbst2_get, sma6201_trm_vbst2_put),
SOC_ENUM_EXT("Trim of minimum on time", sma6201_trm_tmin_enum,
	sma6201_trm_tmin_get, sma6201_trm_tmin_put),

SOC_SINGLE("ADC HPF(1:on_0:off)",
		SMA6201_98_GENERAL_SETTING, 7, 1, 0),
SOC_SINGLE("ADC Phase(1:nml_0:min)",
		SMA6201_98_GENERAL_SETTING, 5, 1, 0),
SOC_ENUM_EXT("ADC Sample Rate", sma6201_adc_sr_enum,
	sma6201_adc_sr_get, sma6201_adc_sr_put),
SOC_SINGLE("ADC OSR DEC(1:64fs_0:128fs)",
		SMA6201_98_GENERAL_SETTING, 1, 1, 0),
SOC_SINGLE("ADC PD(1:PD_0:nml)",
		SMA6201_98_GENERAL_SETTING, 0, 1, 0),

SND_SOC_BYTES_EXT("ADC Digital Vol", 1, adc_digital_vol_get,
	adc_digital_vol_put),

SOC_ENUM_EXT("ADC gain control", sma6201_pgavol_i_enum,
	sma6201_pgavol_i_get, sma6201_pgavol_i_put),
SOC_ENUM_EXT("ADC PTAT resistor control", sma6201_ptat_res_ctrl_enum,
	sma6201_ptat_res_ctrl_get, sma6201_ptat_res_ctrl_put),

SOC_SINGLE("ADC filter and PGA",
		SMA6201_9D_ENABLE_ISENSE, 7, 1, 0),
SOC_SINGLE("ADC modulator",
		SMA6201_9D_ENABLE_ISENSE, 5, 1, 0),
SOC_SINGLE("ADC V I reference",
		SMA6201_9D_ENABLE_ISENSE, 3, 1, 0),
SOC_SINGLE("ADC modulator reset",
		SMA6201_9D_ENABLE_ISENSE, 2, 1, 0),
SOC_SINGLE("ADC chopping clk",
		SMA6201_9D_ENABLE_ISENSE, 1, 1, 0),
SOC_SINGLE("ADC OSR SDM(1:64fs_0:128fs)",
		SMA6201_9D_ENABLE_ISENSE, 0, 1, 0),

SND_SOC_BYTES_EXT("ADC Trim I 1_2", 2,
	trim_isense_current_get, trim_isense_current_put),

SOC_ENUM_EXT("ADC system clk", sma6201_adc_sys_clk_enum,
	sma6201_adc_sys_clk_get, sma6201_adc_sys_clk_put),
SOC_SINGLE("ADC Swap LR(1:swap_0:nml)",
		SMA6201_A0_ADC_MUTE_VOL_CTRL, 5, 1, 0),
SOC_ENUM_EXT("ADC Mute slope", sma6201_adc_mute_slope_enum,
	sma6201_adc_mute_slope_get, sma6201_adc_mute_slope_put),
SOC_SINGLE("ADC Mute switch(1:mute_0:un)",
		SMA6201_A0_ADC_MUTE_VOL_CTRL, 2, 1, 0),
SOC_SINGLE("ADC mode(1:master_0:slave)",
		SMA6201_A0_ADC_MUTE_VOL_CTRL, 2, 1, 0),

SOC_SINGLE("PLL Lock Skip Mode(1:off_0:on)",
		SMA6201_A2_TOP_MAN1, 7, 1, 0),
SOC_SINGLE("PLL Power Down(1:PD_0:op)",
		SMA6201_A2_TOP_MAN1, 6, 1, 0),
SOC_SINGLE("MCLK Select(1:Ext_0:clk)", SMA6201_A2_TOP_MAN1, 5, 1, 0),
SOC_SINGLE("PLL Ref clk1(1:Int_0:Ext)",
		SMA6201_A2_TOP_MAN1, 4, 1, 0),
SOC_SINGLE("PLL Ref clk2(1:SCK_0:Ref clk1)",
		SMA6201_A2_TOP_MAN1, 3, 1, 0),
SOC_SINGLE("DAC DN Conv(1:DC_0:nml)",
		SMA6201_A2_TOP_MAN1, 2, 1, 0),
SOC_SINGLE("SDO Pad Out Ctrl(1:L_0:H)",
		SMA6201_A2_TOP_MAN1, 1, 1, 0),
SOC_SINGLE("SDO Pad Out ctrl2(1:O_0:N)",
		SMA6201_A2_TOP_MAN1, 0, 1, 0),

SOC_SINGLE("Monitor SDO(1:OSC_0:PLL)", SMA6201_A3_TOP_MAN2, 7, 1, 0),
SOC_SINGLE("Test clk(1:clk out_0:nml)",
		SMA6201_A3_TOP_MAN2, 6, 1, 0),
SOC_SINGLE("PLL SDM PD(1:off_0:on)", SMA6201_A3_TOP_MAN2, 5, 1, 0),
SOC_SINGLE("IRQ clear(1:clear_0:nml)",
		SMA6201_A3_TOP_MAN2, 4, 1, 0),
SOC_SINGLE("SDO output(1:high-Z_0:nml)",
		SMA6201_A3_TOP_MAN2, 3, 1, 0),
SOC_SINGLE("BP_SRC(1:MonoMixing_0:nml)",
		SMA6201_A3_TOP_MAN2, 2, 1, 0),
SOC_SINGLE("Clk Monitor(1:off_0:on)",
		SMA6201_A3_TOP_MAN2, 1, 1, 0),
SOC_SINGLE("OSC PD(1:PD_0:nml)",
		SMA6201_A3_TOP_MAN2, 0, 1, 0),

SOC_ENUM_EXT("SDO Output Format", sma6201_o_format_enum,
	sma6201_o_format_get, sma6201_o_format_put),
SOC_ENUM_EXT("SDO SCK rate", sma6201_sck_rate_enum,
	sma6201_sck_rate_get, sma6201_sck_rate_put),
SOC_ENUM_EXT("SDO WD Length", sma6201_wd_length_enum,
	sma6201_wd_length_get, sma6201_wd_length_put),

SOC_SINGLE("TDM clk pol(1:fall_0:rise)",
		SMA6201_A5_TDM1, 7, 1, 0),
SOC_SINGLE("TDM Tx(1:stereo_0:mono)",
		SMA6201_A5_TDM1, 6, 1, 0),
SOC_ENUM_EXT("TDM slot1 pos Rx", sma6201_tdm_slot1_rx_enum,
	sma6201_tdm_slot1_rx_get, sma6201_tdm_slot1_rx_put),
SOC_ENUM_EXT("TDM slot2 pos Rx", sma6201_tdm_slot2_rx_enum,
	sma6201_tdm_slot2_rx_get, sma6201_tdm_slot2_rx_put),

SOC_SINGLE("TDM Data length(1:32_0:16)",
		SMA6201_A6_TDM2, 7, 1, 0),
SOC_SINGLE("TDM n-slot(1:8_0:4)",
		SMA6201_A6_TDM2, 6, 1, 0),
SOC_ENUM_EXT("TDM slot1 pos Tx", sma6201_tdm_slot1_tx_enum,
	sma6201_tdm_slot1_tx_get, sma6201_tdm_slot1_tx_put),
SOC_ENUM_EXT("TDM slot2 pos Tx", sma6201_tdm_slot2_tx_enum,
	sma6201_tdm_slot2_tx_get, sma6201_tdm_slot2_tx_put),

SOC_ENUM_EXT("Clk monitor time select",
	sma6201_test_clock_mon_time_sel_enum,
	sma6201_test_clock_mon_time_sel_get,
	sma6201_test_clock_mon_time_sel_put),
SOC_SINGLE("Clk path select(1:ext_0:sck)",
		SMA6201_A7_TOP_MAN3, 5, 1, 0),
SOC_SINGLE("IRQ_SEL(1:clear_0:nml)",
		SMA6201_A7_TOP_MAN3, 4, 1, 0),
SOC_SINGLE("Test limiter(1:on_0:off)",
		SMA6201_A7_TOP_MAN3, 3, 1, 0),
SOC_SINGLE("SDO IO ctrl(1:out_0:nml)",
		SMA6201_A7_TOP_MAN3, 2, 1, 0),
SOC_SINGLE("Master mode PADs(1:mst_0:slv)",
		SMA6201_A7_TOP_MAN3, 0, 1, 0),

SOC_SINGLE("Piezo Filter(1:off_0:on)",
		SMA6201_A8_TONE_GENERATOR, 7, 1, 0),
SOC_SINGLE("Tone and fine vol(1:bp_0:nml)",
		SMA6201_A8_TONE_GENERATOR, 6, 1, 0),
SOC_SINGLE("Tone audio mix(1:on_0:off)",
		SMA6201_A8_TONE_GENERATOR, 5, 1, 0),
SOC_ENUM_EXT("Tone frequency", sma6201_tone_freq_enum,
	sma6201_tone_freq_get, sma6201_tone_freq_put),
SOC_SINGLE("Tone switch(1:on_0:off)",
		SMA6201_A8_TONE_GENERATOR, 0, 1, 0),

SND_SOC_BYTES_EXT("Tone/Fine Volume", 1,
	tone_fine_volume_get, tone_fine_volume_put),

SND_SOC_BYTES_EXT("PLL_A_D Setting", 2,
	pll_a_d_setting_get, pll_a_d_setting_put),
SND_SOC_BYTES_EXT("PLL LDO Control", 1,
	pll_ldo_ctrl_get, pll_ldo_ctrl_put),

SOC_SINGLE("Sensor input(1:VIN_0:Temp)",
		SMA6201_AD_SPK_OCP_LVL, 7, 1, 0),
SOC_ENUM_EXT("PWM Frequency2", sma6201_pwm_freq_enum,
	sma6201_pwm_freq_get, sma6201_pwm_freq_put),
SOC_ENUM_EXT("SPK OCP Filter time", sma6201_ocp_filter_enum,
	sma6201_ocp_filter_get, sma6201_ocp_filter_put),
SOC_ENUM_EXT("SPK OCP Level", sma6201_ocp_lvl_enum,
	sma6201_ocp_lvl_get, sma6201_ocp_lvl_put),

SOC_SINGLE("SDO Order(1:R_0:N)",
		SMA6201_AE_TOP_MAN4, 7, 1, 0),
SOC_SINGLE("IRQ(1:high-Z_0:nml)", SMA6201_AE_TOP_MAN4, 6, 1, 0),
SOC_ENUM_EXT("SDO Data Selection", sma6201_sdo_data_select_enum,
	sma6201_sdo_data_select_get, sma6201_sdo_data_select_put),
SOC_SINGLE("SDO Data ADC(1:index_0:no)",
		SMA6201_AE_TOP_MAN4, 1, 1, 0),
SOC_SINGLE("SDO Mode ADC(1:24k_0:48k)",
		SMA6201_AE_TOP_MAN4, 0, 1, 0),

SOC_SINGLE("VIN Sense(1:PD_0:nml)",
		SMA6201_AF_VIN_SENSING, 7, 1, 0),
SND_SOC_BYTES_EXT("VIN Sensing", 1, vin_sensing_get, vin_sensing_put),
SOC_SINGLE("SAR clk freq(1:3M_0:1.5M)",
		SMA6201_AF_VIN_SENSING, 0, 1, 0),

SOC_SINGLE("Brown Out(1:on_0:off)",
		SMA6201_B0_BROWN_OUT_P0, 7, 1, 0),
SND_SOC_BYTES_EXT("Brown Out Protect 0_15", 16,
	brown_out_pt_get, brown_out_pt_put),

SOC_ENUM_EXT("Class H mode(Voice_Music_None)",
	voice_music_class_h_mode_enum,
	voice_music_class_h_mode_get, voice_music_class_h_mode_put),
};

static int sma6201_startup(struct snd_soc_component *component)
{
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	if (sma6201->amp_power_status) {
		dev_info(component->dev, "%s : %s\n",
			__func__, "Already AMP Power on");
		return 0;
	}

	dev_info(component->dev, "%s\n", __func__);

	if (sma6201->delayed_shutdown_enable)
		cancel_delayed_work_sync(&sma6201->delayed_shutdown_work);

	/* Please add code when applying external clock */
	if ((sma6201->sys_clk_id != SMA6201_PLL_CLKIN_BCLK) &&
			!(sma6201->ext_clk_status)) {
		dev_info(component->dev, "%s : %s\n",
			__func__, "Applying external clock");

		sma6201->ext_clk_status = true;
	}

	/* PLL LDO bypass enable */
	regmap_update_bits(sma6201->regmap, SMA6201_AC_PLL_CTRL,
			PLL_LDO_BYP_MASK, PLL_LDO_BYP_ENABLE);

	regmap_update_bits(sma6201->regmap, SMA6201_00_SYSTEM_CTRL,
			POWER_MASK, POWER_ON);

	/* Workaround - Defense code to resolve issues that do not change
	 * from low IRQ pin when AMP is powered off
	 */
	regmap_update_bits(sma6201->regmap, SMA6201_AE_TOP_MAN4,
				DIS_IRQ_MASK, NORMAL_OPERATION_IRQ);

	/* Improved boost OCP interrupt issue when turning on the amp */
	msleep(20);

	usleep_range(1000, 1010);

	/* Improved high frequency noise issue when voice call scenario */
	if (sma6201->voice_music_class_h_mode ==
			SMA6201_CLASS_H_VOICE_MODE) {
		regmap_update_bits(sma6201->regmap, SMA6201_A8_TONE_GENERATOR,
			TONE_FREQ_MASK, TONE_FREQ_50);
		regmap_update_bits(sma6201->regmap, SMA6201_03_INPUT1_CTRL3,
			ADD_TONE_VOL_MASK, ADD_TONE_VOL_DECREASE);
		regmap_update_bits(sma6201->regmap, SMA6201_A9_TONE_FINE_VOL,
			TONE_VOL_MASK, TONE_VOL_M_36);
	}

	if (sma6201->stereo_two_chip == true) {
		/* SPK Mode (Stereo) */
		regmap_update_bits(sma6201->regmap, SMA6201_10_SYSTEM_CTRL1,
				SPK_MODE_MASK, SPK_STEREO);
	} else {
		/* SPK Mode (Mono) */
		regmap_update_bits(sma6201->regmap, SMA6201_10_SYSTEM_CTRL1,
				SPK_MODE_MASK, SPK_MONO);
	}

	/* Improved high frequency noise issue when voice call scenario */
	regmap_update_bits(sma6201->regmap, SMA6201_A8_TONE_GENERATOR,
			TONE_ON_MASK, TONE_ON);

	if (sma6201->check_thermal_vbat_enable) {
		if ((sma6201->voice_music_class_h_mode ==
				SMA6201_CLASS_H_MUSIC_MODE)
			&& sma6201->check_thermal_vbat_period > 0) {
			queue_delayed_work(system_freezable_wq,
				&sma6201->check_thermal_vbat_work,
				msecs_to_jiffies(100));
		}
	}

	if (sma6201->check_thermal_fault_enable) {
		if (sma6201->check_thermal_fault_period > 0)
			queue_delayed_work(system_freezable_wq,
				&sma6201->check_thermal_fault_work,
				sma6201->check_thermal_fault_period * HZ);
		else
			queue_delayed_work(system_freezable_wq,
				&sma6201->check_thermal_fault_work,
					CHECK_FAULT_PERIOD_TIME * HZ);
	}

	sma6201->amp_power_status = true;

	regmap_update_bits(sma6201->regmap, SMA6201_0E_MUTE_VOL_CTRL,
				SPK_MUTE_MASK, SPK_UNMUTE);

	return 0;
}

static void sma6201_delayed_shutdown_worker(struct work_struct *work)
{
	struct sma6201_priv *sma6201 =
		container_of(work, struct sma6201_priv,
				delayed_shutdown_work.work);
	unsigned int cur_vol;

	if (sma6201->delayed_shutdown_enable)
		dev_info(sma6201->dev, "%s : %ldsec\n",
			__func__, sma6201->delayed_time_shutdown);

	regmap_update_bits(sma6201->regmap, SMA6201_10_SYSTEM_CTRL1,
			SPK_MODE_MASK, SPK_OFF);

	regmap_update_bits(sma6201->regmap, SMA6201_00_SYSTEM_CTRL,
			POWER_MASK, POWER_OFF);

	regmap_update_bits(sma6201->regmap, SMA6201_A9_TONE_FINE_VOL,
			TONE_VOL_MASK, TONE_VOL_OFF);
	regmap_update_bits(sma6201->regmap, SMA6201_A8_TONE_GENERATOR,
			TONE_ON_MASK, TONE_OFF);

	if (atomic_read(&sma6201->irq_enabled)) {
		disable_irq((unsigned int)sma6201->irq);
		atomic_set(&sma6201->irq_enabled, false);
	}

	/* PLL LDO bypass disable */
	if (sma6201->sys_clk_id == SMA6201_PLL_CLKIN_MCLK
		|| sma6201->sys_clk_id == SMA6201_PLL_CLKIN_BCLK)
		regmap_update_bits(sma6201->regmap,
			SMA6201_AC_PLL_CTRL,
				PLL_LDO_BYP_MASK, PLL_LDO_BYP_DISABLE);

	/* Please add code when removing external clock */
	if ((sma6201->sys_clk_id != SMA6201_PLL_CLKIN_BCLK) &&
			sma6201->ext_clk_status) {
		dev_info(sma6201->dev, "%s : %s\n",
			__func__, "Removing external clock");

		sma6201->ext_clk_status = false;
	}

	if (sma6201->check_thermal_vbat_enable) {
		if ((sma6201->voice_music_class_h_mode ==
				SMA6201_CLASS_H_MUSIC_MODE)
			&& sma6201->check_thermal_vbat_period > 0) {
			/* Only compensation temp for music playback */
			mutex_lock(&sma6201->lock);
			sma6201->threshold_level = 0;

			regmap_read(sma6201->regmap, SMA6201_0A_SPK_VOL,
						&cur_vol);

			if (cur_vol > sma6201->init_vol)
				dev_info(sma6201->dev, "%s : cur vol[%d]  new vol[%d]\n",
				__func__, cur_vol, sma6201->init_vol);
				regmap_write(sma6201->regmap,
					SMA6201_0A_SPK_VOL, sma6201->init_vol);
			mutex_unlock(&sma6201->lock);
		}
	}
}

static int sma6201_shutdown(struct snd_soc_component *component)
{
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	if (!(sma6201->amp_power_status)) {
		dev_info(component->dev, "%s : %s\n",
			__func__, "Already AMP Shutdown");
		return 0;
	}

	dev_info(component->dev, "%s\n", __func__);

	/* Workaround - Defense code to resolve issues that do not change
	 * from low IRQ pin when AMP is powered off
	 */
	regmap_update_bits(sma6201->regmap, SMA6201_AE_TOP_MAN4,
				DIS_IRQ_MASK, HIGH_Z_IRQ);

	regmap_update_bits(sma6201->regmap, SMA6201_0E_MUTE_VOL_CTRL,
				SPK_MUTE_MASK, SPK_MUTE);

	cancel_delayed_work_sync(&sma6201->check_thermal_vbat_work);
	cancel_delayed_work_sync(&sma6201->check_thermal_fault_work);

	/* Mute slope time(15ms) */
	usleep_range(15000, 15010);

	if (sma6201->delayed_shutdown_enable) {
		__pm_wakeup_event(&sma6201->shutdown_wakesrc,
			sma6201->delayed_time_shutdown * HZ);
		queue_delayed_work(system_freezable_wq,
			&sma6201->delayed_shutdown_work,
			sma6201->delayed_time_shutdown * HZ);
	} else {
		queue_delayed_work(system_freezable_wq,
			&sma6201->delayed_shutdown_work, 0);
	}

	sma6201->amp_power_status = false;

	return 0;
}

static int sma6201_clk_supply_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component =
		snd_soc_dapm_to_component(w->dapm);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		dev_info(component->dev, "%s : PRE_PMU\n", __func__);
	break;

	case SND_SOC_DAPM_POST_PMD:
		dev_info(component->dev, "%s : POST_PMD\n", __func__);
	break;
	}

	return 0;
}

static int sma6201_dac_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component =
		snd_soc_dapm_to_component(w->dapm);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		dev_info(component->dev, "%s : PRE_PMU\n", __func__);

		if (sma6201->force_amp_power_down == false)
			sma6201_startup(component);
		break;

	case SND_SOC_DAPM_POST_PMU:
		dev_info(component->dev, "%s : POST_PMU\n", __func__);

		break;

	case SND_SOC_DAPM_PRE_PMD:
		dev_info(component->dev, "%s : PRE_PMD\n", __func__);

		sma6201_shutdown(component);

		break;

	case SND_SOC_DAPM_POST_PMD:
		dev_info(component->dev, "%s : POST_PMD\n", __func__);

		break;
	}

	return 0;
}

static int sma6201_adc_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component =
		snd_soc_dapm_to_component(w->dapm);
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		dev_info(component->dev,
				"%s : DAC/ADC Feedback ON\n", __func__);
		regmap_update_bits(sma6201->regmap,
			SMA6201_09_OUTPUT_CTRL,
				PORT_CONFIG_MASK|PORT_OUT_SEL_MASK,
				OUTPUT_PORT_ENABLE|PIEZO_EQ);
		regmap_update_bits(sma6201->regmap,
			SMA6201_A2_TOP_MAN1, SDO_I2S_CH_MASK,
				SDO_I2S_MONO);
		/* even if Capture stream on, Mixer should turn on
		 * SDO output(1:High-Z,0:Normal output)
		 */
		regmap_update_bits(sma6201->regmap,
			SMA6201_A3_TOP_MAN2, SDO_OUTPUT_MASK,
				NORMAL_OUT);
		regmap_update_bits(sma6201->regmap,
			SMA6201_AE_TOP_MAN4, SDO_DATA_MODE_MASK,
				SDO_DATA_MODE_48K);
		regmap_update_bits(sma6201->regmap,
			SMA6201_98_GENERAL_SETTING, ADC_PD_MASK,
				ADC_OPERATION);
		regmap_update_bits(sma6201->regmap,
			SMA6201_9C_VOLUME_PGA_ISENSE, ADC_PGAVOL_MASK,
				ADC_PGAVOL_X10);
		regmap_update_bits(sma6201->regmap,
			SMA6201_9D_ENABLE_ISENSE, ADC_CHOP_MASK,
				ADC_CHOP_DIS);

		if (sma6201->format == SND_SOC_DAIFMT_DSP_A) {
			regmap_update_bits(sma6201->regmap,
				SMA6201_AE_TOP_MAN4, SDO_DATA_SEL_MASK,
					SDO_DATA_DAC_ADC);
		} else {
			regmap_update_bits(sma6201->regmap,
				SMA6201_AE_TOP_MAN4, SDO_DATA_SEL_MASK,
					SDO_DATA_ADC_DAC_24);
		}
		break;

	case SND_SOC_DAPM_PRE_PMD:
		dev_info(component->dev,
				"%s : DAC/ADC Feedback OFF\n", __func__);
		regmap_update_bits(sma6201->regmap,
			SMA6201_A3_TOP_MAN2, SDO_OUTPUT_MASK,
				HIGH_Z_OUT);
		regmap_update_bits(sma6201->regmap,
			SMA6201_98_GENERAL_SETTING, ADC_PD_MASK,
				ADC_POWER_DOWN);
		break;
	}

	return 0;
}

static const struct snd_soc_dapm_widget sma6201_dapm_widgets[] = {
SND_SOC_DAPM_SUPPLY("CLK_SUPPLY", SND_SOC_NOPM, 0, 0, sma6201_clk_supply_event,
				SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_DAC_E("DAC", "Playback", SND_SOC_NOPM, 0, 0, sma6201_dac_event,
				SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
				SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),
SND_SOC_DAPM_ADC_E("ADC", "Capture", SND_SOC_NOPM, 0, 0,
				sma6201_adc_event,
				SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),
SND_SOC_DAPM_OUTPUT("SPK"),
SND_SOC_DAPM_INPUT("SDO"),
};

static const struct snd_soc_dapm_route sma6201_audio_map[] = {
/* sink, control, source */
{"DAC", NULL, "CLK_SUPPLY"},
{"SPK", NULL, "DAC"},
{"ADC", NULL, "SDO"},
};

static int sma6201_setup_pll(struct snd_soc_component *component,
		struct snd_pcm_hw_params *params)
{
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	int i = 0;
	bool pll_set_flag = false;
	int calc_to_bclk = params_rate(params) * params_physical_width(params)
					* params_channels(params);

	dev_info(component->dev, "%s : rate = %d : bit size = %d : channel = %d\n",
		__func__, params_rate(params), params_physical_width(params),
			params_channels(params));

	/* This setting is valid only for BCM chips that
	 * support only two channels.
	 *
	 * if (sma6201->format == SND_SOC_DAIFMT_DSP_A)
	 *	calc_to_bclk *= 2;
	 */

	if (sma6201->sys_clk_id == SMA6201_PLL_CLKIN_MCLK) {
		/* PLL operation, PLL Clock, External Clock,
		 * PLL reference PLL_REF_CLK1 clock
		 */
		regmap_update_bits(sma6201->regmap, SMA6201_A2_TOP_MAN1,
		PLL_PD_MASK|MCLK_SEL_MASK|PLL_REF_CLK1_MASK|PLL_REF_CLK2_MASK,
		PLL_OPERATION|PLL_CLK|REF_EXTERNAL_CLK|PLL_REF_CLK1);

		for (i = 0; i < sma6201->num_of_pll_matches; i++) {
			if (sma6201->pll_matches[i].input_clk ==
					sma6201->mclk_in) {
				pll_set_flag = true;
				break;
			}
		}
	} else if (sma6201->sys_clk_id == SMA6201_PLL_CLKIN_BCLK) {
		/* SCK clock monitoring mode */
		regmap_update_bits(sma6201->regmap, SMA6201_A7_TOP_MAN3,
				CLOCK_MON_SEL_MASK, CLOCK_MON_SCK);

		/* PLL operation, PLL Clock, External Clock,
		 * PLL reference SCK clock
		 */
		regmap_update_bits(sma6201->regmap, SMA6201_A2_TOP_MAN1,
		PLL_PD_MASK|MCLK_SEL_MASK|PLL_REF_CLK1_MASK|PLL_REF_CLK2_MASK,
		PLL_OPERATION|PLL_CLK|REF_EXTERNAL_CLK|PLL_SCK);

		for (i = 0; i < sma6201->num_of_pll_matches; i++) {
			if (sma6201->pll_matches[i].input_clk ==
					calc_to_bclk) {
				pll_set_flag = true;
				break;
			}
		}
	}
	if (pll_set_flag != true) {
		dev_err(component->dev, "PLL internal table and external clock do not match");
		i = PLL_DEFAULT_SET;
	}

	regmap_write(sma6201->regmap, SMA6201_8B_PLL_POST_N,
			sma6201->pll_matches[i].post_n);
	regmap_write(sma6201->regmap, SMA6201_8C_PLL_N,
			sma6201->pll_matches[i].n);
	regmap_write(sma6201->regmap, SMA6201_8D_PLL_F1,
			sma6201->pll_matches[i].f1);
	regmap_write(sma6201->regmap, SMA6201_8E_PLL_F2,
			sma6201->pll_matches[i].f2);
	regmap_write(sma6201->regmap, SMA6201_8F_PLL_F3_P_CP,
			sma6201->pll_matches[i].f3_p_cp);

	return 0;
}

static int sma6201_dai_hw_params_amp(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	unsigned int input_format = 0;
	bool delayed_shutdown_flag = sma6201->delayed_shutdown_enable;

	dev_info(component->dev, "%s : rate = %d : bit size = %d\n",
		__func__, params_rate(params), params_width(params));

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {

		/* The sigma delta modulation setting for
		 * using the fractional divider in the PLL clock
		 * if (params_format(params) == SNDRV_PCM_FORMAT_S24_LE ||
		 *		params_rate(params) == 44100) {
		 *	regmap_update_bits(sma6201->regmap,
		 *	SMA6201_A3_TOP_MAN2, PLL_SDM_PD_MASK, SDM_ON);
		 * } else {
		 *	regmap_update_bits(sma6201->regmap,
		 *	SMA6201_A3_TOP_MAN2, PLL_SDM_PD_MASK, SDM_OFF);
		 * }
		 */
		/* PLL clock setting according to sample rate and bit */
		if (sma6201->force_amp_power_down == false &&
			(sma6201->sys_clk_id == SMA6201_PLL_CLKIN_MCLK
			|| sma6201->sys_clk_id == SMA6201_PLL_CLKIN_BCLK)) {

			if (sma6201->last_rate != params_rate(params) ||
				sma6201->last_width !=
					params_physical_width(params) ||
				sma6201->last_channel !=
					params_channels(params)) {

				if (sma6201->delayed_shutdown_enable)
					sma6201->delayed_shutdown_enable =
						false;
				sma6201_shutdown(component);
				sma6201->delayed_shutdown_enable =
					delayed_shutdown_flag;

				sma6201_setup_pll(component, params);
				sma6201_startup(component);

				sma6201->last_rate =
					params_rate(params);
				sma6201->last_width =
					params_physical_width(params);
				sma6201->last_channel =
					params_channels(params);
			}
		}

		if (sma6201->force_amp_power_down == false &&
			!atomic_read(&sma6201->irq_enabled)) {
			enable_irq((unsigned int)sma6201->irq);
			irq_set_irq_wake(sma6201->irq, 1);

			if (device_may_wakeup(sma6201->dev))
				enable_irq_wake(sma6201->irq);

			atomic_set(&sma6201->irq_enabled, true);
		}

		switch (params_rate(params)) {
		case 8000:
		case 12000:
		case 16000:
		case 24000:
		case 32000:
		case 44100:
		case 48000:
		case 96000:
		regmap_update_bits(sma6201->regmap, SMA6201_A2_TOP_MAN1,
				DAC_DN_CONV_MASK, DAC_DN_CONV_DISABLE);
		regmap_update_bits(sma6201->regmap, SMA6201_01_INPUT1_CTRL1,
				LEFTPOL_MASK, LOW_FIRST_CH);
		break;

		case 192000:
		regmap_update_bits(sma6201->regmap, SMA6201_A2_TOP_MAN1,
				DAC_DN_CONV_MASK, DAC_DN_CONV_ENABLE);
		regmap_update_bits(sma6201->regmap, SMA6201_01_INPUT1_CTRL1,
				LEFTPOL_MASK, HIGH_FIRST_CH);
		break;

		default:
			dev_err(component->dev, "%s not support rate : %d\n",
				__func__, params_rate(params));

		return -EINVAL;
		}

		/* Setting TDM Rx operation */
		if (sma6201->format == SND_SOC_DAIFMT_DSP_A) {
			regmap_update_bits(sma6201->regmap,
				SMA6201_A4_SDO_OUT_FMT,
				O_FORMAT_MASK, O_FORMAT_TDM);

			switch (params_physical_width(params)) {
			case 16:
			regmap_update_bits(sma6201->regmap, SMA6201_A6_TDM2,
					TDM_DL_MASK, TDM_DL_16);
			break;
			case 32:
			regmap_update_bits(sma6201->regmap, SMA6201_A6_TDM2,
					TDM_DL_MASK, TDM_DL_32);
			break;
			default:
			dev_err(component->dev, "%s not support TDM %d bit\n",
				__func__, params_physical_width(params));
			}

			switch (params_channels(params)) {
			case 4:
			regmap_update_bits(sma6201->regmap, SMA6201_A6_TDM2,
					TDM_N_SLOT_MASK, TDM_N_SLOT_4);
			break;
			case 8:
			regmap_update_bits(sma6201->regmap, SMA6201_A6_TDM2,
					TDM_N_SLOT_MASK, TDM_N_SLOT_8);
			break;
			default:
			dev_err(component->dev, "%s not support TDM %d channel\n",
				__func__, params_channels(params));
			}
			/* Select a slot to process TDM Rx data
			 * (Default slot0, slot1)
			 */
			switch (params_physical_width(params)) {
			case 16:
			regmap_update_bits(sma6201->regmap, SMA6201_A5_TDM1,
					TDM_16BIT_SLOT1_RX_POS_MASK,
					TDM_16BIT_SLOT1_RX_POS_0);
			regmap_update_bits(sma6201->regmap, SMA6201_A5_TDM1,
					TDM_16BIT_SLOT2_RX_POS_MASK,
					TDM_16BIT_SLOT2_RX_POS_1);
			break;
			case 32:
			regmap_update_bits(sma6201->regmap, SMA6201_A5_TDM1,
					TDM_32BIT_SLOT1_RX_POS_MASK,
					TDM_32BIT_SLOT1_RX_POS_0);
			regmap_update_bits(sma6201->regmap, SMA6201_A5_TDM1,
					TDM_32BIT_SLOT2_RX_POS_MASK,
					TDM_32BIT_SLOT2_RX_POS_1);
			}
		}
	/* Substream->stream is SNDRV_PCM_STREAM_CAPTURE */
	} else {

		switch (params_format(params)) {

		case SNDRV_PCM_FORMAT_S16_LE:
			dev_info(component->dev,
				"%s set format SNDRV_PCM_FORMAT_S16_LE\n",
				__func__);
			regmap_update_bits(sma6201->regmap,
				SMA6201_A4_SDO_OUT_FMT, WD_LENGTH_MASK,
					WL_16BIT);
			regmap_update_bits(sma6201->regmap,
				SMA6201_A4_SDO_OUT_FMT, SCK_RATE_MASK,
					SCK_RATE_32FS);
			break;

		case SNDRV_PCM_FORMAT_S24_LE:
			dev_info(component->dev,
				"%s set format SNDRV_PCM_FORMAT_S24_LE\n",
				__func__);
			regmap_update_bits(sma6201->regmap,
				SMA6201_A4_SDO_OUT_FMT, WD_LENGTH_MASK,
					WL_24BIT);
			regmap_update_bits(sma6201->regmap,
				SMA6201_A4_SDO_OUT_FMT, SCK_RATE_MASK,
					SCK_RATE_64FS);

			break;

		default:
			dev_err(component->dev,
				"%s not support data bit : %d\n", __func__,
						params_format(params));
			return -EINVAL;
		}

		/* Setting TDM Tx operation */
		if (sma6201->format == SND_SOC_DAIFMT_DSP_A) {
			regmap_update_bits(sma6201->regmap, SMA6201_A5_TDM1,
					TDM_CLK_POL_MASK, TDM_CLK_POL_RISE);
			regmap_update_bits(sma6201->regmap, SMA6201_A5_TDM1,
					TDM_TX_MODE_MASK, TDM_TX_STEREO);
			/* Select a slot to process TDM Tx data
			 * (Default slot0, slot1)
			 */
			regmap_update_bits(sma6201->regmap, SMA6201_A6_TDM2,
					TDM_SLOT1_TX_POS_MASK,
					TDM_SLOT1_TX_POS_0);
			regmap_update_bits(sma6201->regmap, SMA6201_A6_TDM2,
					TDM_SLOT2_TX_POS_MASK,
					TDM_SLOT2_TX_POS_1);
		}
	}

	switch (params_width(params)) {
	case 16:
		switch (sma6201->format) {
		case SND_SOC_DAIFMT_I2S:
			input_format |= STANDARD_I2S;
			break;
		case SND_SOC_DAIFMT_LEFT_J:
			input_format |= LJ;
			break;
		case SND_SOC_DAIFMT_RIGHT_J:
			input_format |= RJ_16BIT;
			break;
		}
		break;
	case 24:
		switch (sma6201->format) {
		case SND_SOC_DAIFMT_I2S:
			input_format |= STANDARD_I2S;
			break;
		case SND_SOC_DAIFMT_LEFT_J:
			input_format |= LJ;
			break;
		case SND_SOC_DAIFMT_RIGHT_J:
			input_format |= RJ_24BIT;
			break;
		}
		break;

	default:
		dev_err(component->dev,
			"%s not support data bit : %d\n", __func__,
					params_format(params));
		return -EINVAL;
	}

	regmap_update_bits(sma6201->regmap, SMA6201_01_INPUT1_CTRL1,
				I2S_MODE_MASK, input_format);

	return 0;
}

static int sma6201_dai_set_sysclk_amp(struct snd_soc_dai *dai,
			int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_component *component = dai->component;
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	dev_info(component->dev, "%s\n", __func__);

	/* Requested clock frequency is already setup */
	if (freq == sma6201->mclk_in)
		return 0;

	switch (clk_id) {
	case SMA6201_EXTERNAL_CLOCK_19_2:
		regmap_update_bits(sma6201->regmap, SMA6201_00_SYSTEM_CTRL,
				CLKSYSTEM_MASK, EXT_19_2);
		break;

	case SMA6201_EXTERNAL_CLOCK_24_576:
		regmap_update_bits(sma6201->regmap, SMA6201_00_SYSTEM_CTRL,
				CLKSYSTEM_MASK, EXT_24_576);
		break;
	case SMA6201_PLL_CLKIN_MCLK:
		if (freq < 1536000 || freq > 24576000) {
			/* out of range PLL_CLKIN, fall back to use BCLK */
			dev_warn(component->dev, "Out of range PLL_CLKIN: %u\n",
				freq);
			clk_id = SMA6201_PLL_CLKIN_BCLK;
			freq = 0;
		}
	case SMA6201_PLL_CLKIN_BCLK:
		break;
	default:
		dev_err(component->dev, "Invalid clk id: %d\n", clk_id);
		return -EINVAL;
	}
	sma6201->sys_clk_id = clk_id;
	sma6201->mclk_in = freq;
	return 0;
}

static int sma6201_dai_digital_mute(struct snd_soc_dai *component_dai, int mute)
{
	struct snd_soc_component *component = component_dai->component;
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	if (!(sma6201->amp_power_status)) {
		dev_info(component->dev, "%s : %s\n",
			__func__, "Already AMP Shutdown");
		return 0;
	}

	if (mute) {
		dev_info(component->dev, "%s : %s\n", __func__, "MUTE");

		regmap_update_bits(sma6201->regmap, SMA6201_0E_MUTE_VOL_CTRL,
					SPK_MUTE_MASK, SPK_MUTE);

	} else {
		dev_info(component->dev, "%s : %s\n", __func__, "UNMUTE");

		regmap_update_bits(sma6201->regmap, SMA6201_0E_MUTE_VOL_CTRL,
					SPK_MUTE_MASK, SPK_UNMUTE);
	}

	return 0;
}

static int sma6201_dai_set_fmt_amp(struct snd_soc_dai *component_dai,
					unsigned int fmt)
{
	struct snd_soc_component *component = component_dai->component;
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {

	case SND_SOC_DAIFMT_CBS_CFS:
		dev_info(component->dev, "%s : %s\n", __func__, "Slave mode");
		/* I2S/TDM clock mode - Slave mode */
		regmap_update_bits(sma6201->regmap, SMA6201_01_INPUT1_CTRL1,
					MASTER_SLAVE_MASK, SLAVE_MODE);
		regmap_update_bits(sma6201->regmap, SMA6201_A7_TOP_MAN3,
					MAS_EN_MASK, MAS_EN_SLAVE);
		break;

	case SND_SOC_DAIFMT_CBM_CFM:
		dev_info(component->dev, "%s : %s\n", __func__, "Master mode");
		/* I2S/TDM clock mode - Master mode */
		regmap_update_bits(sma6201->regmap, SMA6201_01_INPUT1_CTRL1,
					MASTER_SLAVE_MASK, MASTER_MODE);
		regmap_update_bits(sma6201->regmap, SMA6201_A7_TOP_MAN3,
					MAS_EN_MASK, MAS_EN_MASTER);
		break;

	default:
		dev_err(component->dev,
				"Unsupported Master/Slave : 0x%x\n", fmt);
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {

	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_RIGHT_J:
	case SND_SOC_DAIFMT_LEFT_J:
	case SND_SOC_DAIFMT_DSP_A:
	case SND_SOC_DAIFMT_DSP_B:
		sma6201->format = fmt & SND_SOC_DAIFMT_FORMAT_MASK;
		break;
	default:
		dev_err(component->dev,
			"Unsupported Audio Interface Format : 0x%x\n", fmt);
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {

	case SND_SOC_DAIFMT_IB_NF:
		dev_info(component->dev, "%s : %s\n",
			__func__, "Invert BCLK + Normal Frame");
		regmap_update_bits(sma6201->regmap, SMA6201_01_INPUT1_CTRL1,
					SCK_RISING_MASK, SCK_RISING_EDGE);
		break;
	case SND_SOC_DAIFMT_IB_IF:
		dev_info(component->dev, "%s : %s\n",
			__func__, "Invert BCLK + Invert Frame");
		regmap_update_bits(sma6201->regmap, SMA6201_01_INPUT1_CTRL1,
					LEFTPOL_MASK|SCK_RISING_MASK,
					HIGH_FIRST_CH|SCK_RISING_EDGE);
		break;
	case SND_SOC_DAIFMT_NB_IF:
		dev_info(component->dev, "%s : %s\n",
			__func__, "Normal BCLK + Invert Frame");
		regmap_update_bits(sma6201->regmap, SMA6201_01_INPUT1_CTRL1,
					LEFTPOL_MASK, HIGH_FIRST_CH);
		break;
	case SND_SOC_DAIFMT_NB_NF:
		dev_info(component->dev, "%s : %s\n",
			__func__, "Normal BCLK + Normal Frame");
		break;
	default:
		dev_err(component->dev,
			"Unsupported Bit & Frameclock : 0x%x\n", fmt);
		return -EINVAL;
	}

	return 0;
}

static const struct snd_soc_dai_ops sma6201_dai_ops_amp = {
	.set_sysclk = sma6201_dai_set_sysclk_amp,
	.set_fmt = sma6201_dai_set_fmt_amp,
	.hw_params = sma6201_dai_hw_params_amp,
	.digital_mute = sma6201_dai_digital_mute,
};

#define SMA6201_RATES SNDRV_PCM_RATE_8000_192000
#define SMA6201_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | \
			SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver sma6201_dai[] = {
{
	.name = "sma6201-piezo",
	.id = 0,
	.playback = {
	.stream_name = "Playback",
	.channels_min = 1,
	.channels_max = 8,
	.rates = SMA6201_RATES,
	.formats = SMA6201_FORMATS,
	},
	.capture = {
	.stream_name = "Capture",
	.channels_min = 1,
	.channels_max = 8,
	.rates = SMA6201_RATES,
	.formats = SMA6201_FORMATS,
	},
	.ops = &sma6201_dai_ops_amp,
}
};

static int sma6201_set_bias_level(struct snd_soc_component *component,
			enum snd_soc_bias_level level)
{
	switch (level) {
	case SND_SOC_BIAS_ON:

		dev_info(component->dev, "%s\n", "SND_SOC_BIAS_ON");
		sma6201_startup(component);

		break;

	case SND_SOC_BIAS_PREPARE:

		dev_info(component->dev, "%s\n", "SND_SOC_BIAS_PREPARE");

		break;

	case SND_SOC_BIAS_STANDBY:

		dev_info(component->dev, "%s\n", "SND_SOC_BIAS_STANDBY");

		break;

	case SND_SOC_BIAS_OFF:

		dev_info(component->dev, "%s\n", "SND_SOC_BIAS_OFF");
		sma6201_shutdown(component);

		break;
	}

	return 0;
}

static irqreturn_t sma6201_isr(int irq, void *data)
{
	struct sma6201_priv *sma6201 = (struct sma6201_priv *) data;
	int ret;
	unsigned int over_temp, ocp_val, sar_adc, bop_state;

	ret = regmap_read(sma6201->regmap, SMA6201_FA_STATUS1, &over_temp);
	if (ret != 0) {
		dev_err(sma6201->dev,
			"failed to read SMA6201_FA_STATUS1 : %d\n", ret);
	}

	ret = regmap_read(sma6201->regmap, SMA6201_FB_STATUS2, &ocp_val);
	if (ret != 0) {
		dev_err(sma6201->dev,
			"failed to read SMA6201_FB_STATUS2 : %d\n", ret);
	}

	ret = regmap_read(sma6201->regmap, SMA6201_FC_STATUS3, &sar_adc);
	if (ret != 0) {
		dev_err(sma6201->dev,
			"failed to read SMA6201_FC_STATUS3 : %d\n", ret);
	}

	ret = regmap_read(sma6201->regmap, SMA6201_FE_STATUS5, &bop_state);
	if (ret != 0) {
		dev_err(sma6201->dev,
			"failed to read SMA6201_FE_STATUS5 : %d\n", ret);
		return IRQ_HANDLED;
	}

	dev_crit(sma6201->dev,
			"%s : SAR_ADC : %x\n", __func__, sar_adc);
	dev_crit(sma6201->dev,
			"%s : BOP_STATE : %d\n", __func__, bop_state);

	if (~over_temp & OT2_OK_STATUS)
		dev_crit(sma6201->dev,
			"%s : OT2(Over Temperature Level 2)\n", __func__);
	if (ocp_val & OCP_SPK_STATUS) {
		dev_crit(sma6201->dev,
			"%s : OCP_SPK(Over Current Protect SPK)\n", __func__);
		if (sma6201->enable_ocp_aging) {
			mutex_lock(&sma6201->lock);
			sma6201_thermal_compensation(sma6201, true);
			mutex_unlock(&sma6201->lock);
		}
		sma6201->ocp_count++;
	}
	if (ocp_val & OCP_BST_STATUS) {
		dev_crit(sma6201->dev,
			"%s : OCP_BST(Over Current Protect Boost)\n", __func__);
		sma6201->ocp_count++;
	}
	if (ocp_val & UVLO_BST_STATUS)
		dev_crit(sma6201->dev,
			"%s : UVLO(Under Voltage Lock Out)\n", __func__);
	if (ocp_val & CLOCK_MON_STATUS)
		dev_crit(sma6201->dev,
			"%s : CLK_FAULT(No clock input)\n", __func__);
	if ((ocp_val & OCP_SPK_STATUS) || (ocp_val & OCP_BST_STATUS))
		dev_crit(sma6201->dev, "%s : OCP has occurred < %d > times\n",
				__func__, sma6201->ocp_count);

	return IRQ_HANDLED;
}

static void sma6201_check_thermal_fault_worker(struct work_struct *work)
{
	struct sma6201_priv *sma6201 =
		container_of(work, struct sma6201_priv,
				check_thermal_fault_work.work);
	int ret;
	unsigned int over_temp, sar_adc, bop_state;
	unsigned int bop_threshold = 143;

	ret = regmap_read(sma6201->regmap, SMA6201_FA_STATUS1, &over_temp);
	if (ret != 0) {
		dev_err(sma6201->dev,
			"failed to read SMA6201_FA_STATUS1 : %d\n", ret);
		return;
	}
	ret = regmap_read(sma6201->regmap, SMA6201_FC_STATUS3, &sar_adc);
	if (ret != 0) {
		dev_err(sma6201->dev,
			"failed to read SMA6201_FC_STATUS3 : %d\n", ret);
	}

	ret = regmap_read(sma6201->regmap, SMA6201_FE_STATUS5, &bop_state);
	if (ret != 0) {
		dev_err(sma6201->dev,
			"failed to read SMA6201_FE_STATUS5 : %d\n", ret);
	}

	if (bop_state != 0 || sar_adc <= bop_threshold) {
		/* Expected brown out operation */
		dev_info(sma6201->dev,
			"%s : SAR_ADC : %x, BOP_STATE : %d\n",
				__func__, sar_adc, bop_state);
	}

	if (~over_temp & OT1_OK_STATUS) {
		dev_info(sma6201->dev,
			"%s : OT1(Over Temperature Level 1)\n", __func__);
	}

	if (sma6201->check_thermal_fault_enable) {
		if (sma6201->check_thermal_fault_period > 0)
			queue_delayed_work(system_freezable_wq,
				&sma6201->check_thermal_fault_work,
				sma6201->check_thermal_fault_period * HZ);
		else
			queue_delayed_work(system_freezable_wq,
				&sma6201->check_thermal_fault_work,
					CHECK_FAULT_PERIOD_TIME * HZ);
	}
}

static void sma6201_check_thermal_vbat_worker(struct work_struct *work)
{
	struct sma6201_priv *sma6201 =
		container_of(work, struct sma6201_priv,
			check_thermal_vbat_work.work);
#ifdef CONFIG_SMA6201_BATTERY_READING
	union power_supply_propval prop = {0, };
	int ret = 0;
#endif
	struct outside_status fifo_buf_in = {0, };

	mutex_lock(&sma6201->lock);

	if (sma6201->thermal_sense_opt == -1) {

#ifndef CONFIG_MACH_PIEZO
		sma6201->tz_sense =
			thermal_zone_get_zone_by_name("quiet_therm");
#else
		/* Function for checking thermal
		 * quiet_therm : skin-therm,
		 * piezo_therm : wp-therm, vts : vts-virt-therm
		 */
		sma6201->tz_sense =
			thermal_zone_get_zone_by_name("skin-therm");
#endif
	} else {

#ifndef CONFIG_MACH_PIEZO
		sma6201->tz_sense =
			thermal_zone_get_zone_by_name("quiet_therm");
#else
		/* Function for checking thermal
		 * quiet_therm : skin-therm,
		 * piezo_therm : wp-therm, vts : vts-virt-therm
		 */
		switch (sma6201->thermal_sense_opt) {
		case 1:
			sma6201->tz_sense =
				thermal_zone_get_zone_by_name("skin-therm");
			break;
		case 2:
			sma6201->tz_sense =
				thermal_zone_get_zone_by_name("wp-therm");
			break;
		default:
			sma6201->tz_sense =
				thermal_zone_get_zone_by_name("skin-therm");
			break;
		}
#endif
	}

	if (IS_ERR(sma6201->tz_sense))
		dev_info(sma6201->dev, "%s : need to check thermal zone name:%p\n",
			__func__, sma6201->tz_sense);
	else
		thermal_zone_get_temp(sma6201->tz_sense,
			&fifo_buf_in.thermal_deg);

#ifdef CONFIG_MACH_PIEZO
	/* Converting xxxxx mC to xx.x C */
	fifo_buf_in.thermal_deg = fifo_buf_in.thermal_deg/100;
#else
	fifo_buf_in.thermal_deg = fifo_buf_in.thermal_deg*10;
#endif

/* Currently not checked Battery level */
#ifdef CONFIG_SMA6201_BATTERY_READING
	sma6201->batt_psy = power_supply_get_by_name("battery");

	if (!sma6201->batt_psy) {
		pr_err("failed get batt_psy\n");
		goto exit_vbat_worker;
	}
	ret = power_supply_get_property(sma6201->batt_psy,
		POWER_SUPPLY_PROP_VOLTAGE_NOW, &prop);
	if (ret < 0) {
		pr_err("Error in getting battery voltage, ret=%d\n", ret);
		fifo_buf_in.batt_voltage_mV = 4450;
	} else
		fifo_buf_in.batt_voltage_mV = prop.intval;
#endif

	fifo_buf_in.id = sma6201->fifo_count;

	if (!kfifo_is_full(&sma6201->data_fifo)) {
		kfifo_in(&sma6201->data_fifo,
			(unsigned char *)&fifo_buf_in,
			sizeof(fifo_buf_in));

		sma6201->fifo_count++;
		pr_debug("%s :queue in", __func__);
	}

#ifdef CONFIG_SMA6201_BATTERY_READING
	dev_dbg(sma6201->dev,
	"%s : id - [%d]  sense_temp - [%3d] deg bat_vol - [%d] mV\n",
	__func__, fifo_buf_in.id,
	fifo_buf_in.thermal_deg,
	fifo_buf_in.batt_voltage_mV/1000);
#else
	dev_dbg(sma6201->dev,
	"%s : id - [%d]  sense_temp - [%3d]\n",
	__func__, fifo_buf_in.id,
	fifo_buf_in.thermal_deg);
#endif
	sma6201_thermal_compensation(sma6201, false);

#ifdef CONFIG_SMA6201_BATTERY_READING
exit_vbat_worker:
#endif

	if (sma6201->check_thermal_vbat_enable) {
		if (sma6201->check_thermal_vbat_period > 0)
			queue_delayed_work(system_freezable_wq,
				&sma6201->check_thermal_vbat_work,
				sma6201->check_thermal_vbat_period * HZ);
		else
			queue_delayed_work(system_freezable_wq,
				&sma6201->check_thermal_vbat_work,
					CHECK_COMP_PERIOD_TIME * HZ);
	}
	mutex_unlock(&sma6201->lock);
}

static int sma6201_thermal_compensation(struct sma6201_priv *sma6201,
		bool ocp_status)
{
	unsigned int cur_vol;
	int ret, i = 0;
	struct outside_status fifo_buf_out = {0, };
	int vbat_gain = 0, vbat_status;

	/* SPK OCP issued or monitoring function */
	if (ocp_status) {
		i = sma6201->threshold_level;
		sma6201->temp_match[i].ocp_count++;

		if (i == 0) {
			dev_info(sma6201->dev, "%s : OCP occured in normal temp\n",
				__func__);
		} else {
			if (sma6201->enable_ocp_aging) {
				/* Volume control (0dB/0x30) */
				regmap_read(sma6201->regmap, SMA6201_0A_SPK_VOL,
					&cur_vol);

				sma6201->temp_match[i].comp_gain++;
				cur_vol = sma6201->init_vol +
					sma6201->temp_match[i].comp_gain;
				regmap_write(sma6201->regmap,
					SMA6201_0A_SPK_VOL, cur_vol);
			}
		}
		/* Need to update compensation gain */
		dev_info(sma6201->dev,
			"%s :OCP occured in TEMP[%d] GAIN_C[%d] OCP_N[%d] HIT_N[%d] ACT[%d]\n",
			__func__, sma6201->temp_match[i].thermal_limit,
			sma6201->temp_match[i].comp_gain,
			sma6201->temp_match[i].ocp_count,
			sma6201->temp_match[i].hit_count,
			sma6201->temp_match[i].activate);

		return 0;
	}

	if (!kfifo_is_empty(&sma6201->data_fifo)) {
		ret = kfifo_out(&sma6201->data_fifo,
			(unsigned char *)&fifo_buf_out,
			sizeof(fifo_buf_out));

		dev_dbg(sma6201->dev, "%s :queue out\n", __func__);

		if (ret != sizeof(fifo_buf_out))
			return ret;
#ifdef CONFIG_SMA6201_BATTERY_READING
		dev_dbg(sma6201->dev,
		"%s : id - [%d]  sense_temp - [%3d]  deg bat_vol - %d mV\n",
		__func__, fifo_buf_out.id,
		fifo_buf_out.thermal_deg,
		fifo_buf_out.batt_voltage_mV/1000);
#else
		dev_dbg(sma6201->dev,
		"%s : id - [%d]  sense_temp - [%3d]  deg\n",
		__func__, fifo_buf_out.id,
		fifo_buf_out.thermal_deg);
#endif
	}

	for (i = 0; i < sma6201->num_of_temperature_matches; i++) {
		/* Check  matching temperature
		 * compare current temp & table
		 */
		if ((fifo_buf_out.thermal_deg <
			 sma6201->temp_match[i].thermal_limit)) {
			dev_dbg(sma6201->dev,
				"%s :Matched TEMP[%d] GAIN_C[%d] OCP_N[%d] HIT_N[%d] ACT[%d]\n",
				__func__,
				sma6201->temp_match[i].thermal_limit,
				sma6201->temp_match[i].comp_gain,
				sma6201->temp_match[i].ocp_count,
				sma6201->temp_match[i].hit_count,
				sma6201->temp_match[i].activate);
			break;
		}
	}

	if (vbat_status != -1 &&
		vbat_status < VBAT_TABLE_NUM) {
		vbat_gain =
			sma6201_vbat_gain_matches[vbat_status].comp_gain;
	}

	/* Updating the gain for battery level and temperature */
	if (i == 0 || (sma6201->temp_match[i].activate == 0)) {
		/* Matched normal temeperature in table */
		dev_dbg(sma6201->dev, "%s :temp[%d] matched in normal temperature\n",
		__func__, i);

		if (vbat_gain > 0) {
			/* Prefered battery level in normal temperature */
			cur_vol = sma6201->init_vol + vbat_gain;
			regmap_write(sma6201->regmap, SMA6201_0A_SPK_VOL,
				cur_vol);
			dev_info(sma6201->dev, "%s : low battery gain[%d] in normal temp\n",
			__func__, cur_vol);
		} else if (sma6201->threshold_level != i) {
			/* Normal gain */
			regmap_write(sma6201->regmap, SMA6201_0A_SPK_VOL,
				sma6201->init_vol);
		}
	} else if (i < sma6201->num_of_temperature_matches) {

		/* Matched temeperature in table */
		dev_dbg(sma6201->dev, "%s :temp[%d] matched", __func__, i);
		sma6201->temp_match[i].hit_count++;

		if (sma6201->threshold_level != i) {
			/* First step, only tracking temperature
			 * need to optimise for temp rising and falling slope
			 */
			if (vbat_gain > sma6201->temp_match[i].comp_gain) {
				/* Case Battery gain comp */
				cur_vol = sma6201->init_vol + vbat_gain;
				regmap_write(sma6201->regmap,
					SMA6201_0A_SPK_VOL, cur_vol);
			} else {
				/* Temp comp */
				cur_vol = sma6201->init_vol +
					sma6201->temp_match[i].comp_gain;
				regmap_write(sma6201->regmap,
					SMA6201_0A_SPK_VOL, cur_vol);
			}
			dev_info(sma6201->dev, "%s : cur temp[%d]  previous temp[%d] gain[%d]\n",
			__func__, i, sma6201->threshold_level, cur_vol);

		} else if (vbat_gain > sma6201->temp_match[i].comp_gain) {
			/* Temperature is not changed
			 * Only battery gain comp
			 */
			dev_info(sma6201->dev,
				"%s : cur temp[%d] - only vbat gain[%d] comp\n",
				__func__, i, vbat_gain);
			cur_vol = sma6201->init_vol + vbat_gain;
			regmap_write(sma6201->regmap,
				SMA6201_0A_SPK_VOL, cur_vol);
		}
	}
	/* Updating previous temperature */
	sma6201->threshold_level = i;

	return 0;
}

#ifdef CONFIG_PM
static int sma6201_suspend(struct snd_soc_component *component)
{

	dev_info(component->dev, "%s\n", __func__);

	return 0;
}

static int sma6201_resume(struct snd_soc_component *component)
{

	dev_info(component->dev, "%s\n", __func__);

	return 0;
}
#else
#define sma6201_suspend NULL
#define sma6201_resume NULL
#endif

static int sma6201_reset(struct snd_soc_component *component)
{
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	struct reg_default *reg_val;
	int cnt, ret;
	unsigned int status;
	int eq_len = sma6201->eq_reg_array_len / sizeof(uint32_t);
	int bo_len = sma6201->bo_reg_array_len / sizeof(uint32_t);

	dev_info(component->dev, "%s\n", __func__);

	ret = regmap_read(sma6201->regmap, SMA6201_FF_VERSION, &status);

	if (ret != 0)
		dev_err(sma6201->dev,
			"failed to read SMA6201_FF_VERSION : %d\n", ret);
	else
		sma6201->rev_num = status & REV_NUM_STATUS;

	dev_info(component->dev, "SMA6201 chip revision ID - %d\n",
			sma6201->rev_num);

	/* External clock 24.576MHz */
	regmap_write(sma6201->regmap, SMA6201_00_SYSTEM_CTRL, 0x80);
	/* Volume control (0dB/0x30) */
	regmap_write(sma6201->regmap, SMA6201_0A_SPK_VOL, sma6201->init_vol);
	/* VOL_SLOPE - Fast Volume Slope,
	 * MUTE_SLOPE - Fast Mute Slope, SPK_MUTE - muted
	 */
	regmap_write(sma6201->regmap, SMA6201_0E_MUTE_VOL_CTRL,	0xFF);

	/* Bass Off & EQ Enable
	 * MONO_MIX Off(TW) for SPK Signal Path
	 */
	regmap_write(sma6201->regmap, SMA6201_11_SYSTEM_CTRL2, 0xA0);

	if (sma6201->stereo_two_chip == true) {
		/* MONO MIX Off */
		regmap_update_bits(sma6201->regmap,
		SMA6201_11_SYSTEM_CTRL2, MONOMIX_MASK, MONOMIX_OFF);
	} else {
		/* MONO MIX ON */
		regmap_update_bits(sma6201->regmap,
		SMA6201_11_SYSTEM_CTRL2, MONOMIX_MASK, MONOMIX_ON);
	}

	/* Stereo idle noise improvement, FDPEC Gain - 4,
	 * HDC OPAMP Current - 80uA
	 */
	regmap_write(sma6201->regmap, SMA6201_13_FDPEC_CTRL1, 0x29);

	if (sma6201->rev_num == REV_NUM_REV0) {
		/* Delay control between OUTA and
		 * OUTB with main clock duty cycle
		 */
		regmap_write(sma6201->regmap, SMA6201_14_MODULATOR, 0x61);
	} else {
		/* Delay control between OUTA and
		 * OUTB with main clock duty cycle
		 */
		regmap_write(sma6201->regmap, SMA6201_14_MODULATOR, 0x0D);
	}

	/* HPF Frequency - 201 Hz */
	regmap_write(sma6201->regmap, SMA6201_15_BASS_SPK1,	0x06);
	regmap_write(sma6201->regmap, SMA6201_16_BASS_SPK2,	0x05);
	regmap_write(sma6201->regmap, SMA6201_17_BASS_SPK3,	0x05);
	regmap_write(sma6201->regmap, SMA6201_18_BASS_SPK4,	0x0E);
	regmap_write(sma6201->regmap, SMA6201_19_BASS_SPK5,	0x61);
	regmap_write(sma6201->regmap, SMA6201_1A_BASS_SPK6,	0x0B);
	regmap_write(sma6201->regmap, SMA6201_1B_BASS_SPK7,	0x06);
	regmap_write(sma6201->regmap, SMA6201_21_DGC,		0x96);

	if (sma6201->rev_num == REV_NUM_REV0) {
		/* Prescaler Enable, -0.25dB Pre Gain */
		regmap_write(sma6201->regmap, SMA6201_22_PRESCALER, 0x2C);
	} else {
		/* Prescaler Bypass */
		regmap_write(sma6201->regmap, SMA6201_22_PRESCALER, 0x2D);
	}

	regmap_write(sma6201->regmap, SMA6201_23_COMP_LIM1, 0x1F);
	regmap_write(sma6201->regmap, SMA6201_24_COMP_LIM2, 0x02);
	regmap_write(sma6201->regmap, SMA6201_25_COMP_LIM3, 0x09);
	regmap_write(sma6201->regmap, SMA6201_26_COMP_LIM4, 0xFF);

	/* Disable Battery Overvoltage, Disable Return Current Control */
	regmap_write(sma6201->regmap, SMA6201_27_RET_CUR_CTRL, 0x00);

	regmap_write(sma6201->regmap, SMA6201_2B_EQ_MODE,		0x17);
	regmap_write(sma6201->regmap, SMA6201_2C_EQBAND1_BYP,	0x0C);
	regmap_write(sma6201->regmap, SMA6201_2D_EQBAND2_BYP,	0x0C);
	regmap_write(sma6201->regmap, SMA6201_2E_EQBAND3_BYP,	0x0C);
	regmap_write(sma6201->regmap, SMA6201_2F_EQBAND4_BYP,	0x0C);
	regmap_write(sma6201->regmap, SMA6201_30_EQBAND5_BYP,	0x0C);

	/* PWM Slope control, PWM Dead time control */
	regmap_write(sma6201->regmap, SMA6201_37_SLOPE_CTRL, 0x05);

	if (sma6201->rev_num == REV_NUM_REV0) {
		/* Feedback gain trimming - No trimming,
		 * Recovery Current Control Mode - Normal,
		 * Recovery Current Control Enable,
		 * PWM frequency - 740kHz,
		 * Differential OPAMP bias current - 80uA
		 */
		regmap_write(sma6201->regmap, SMA6201_92_FDPEC_CTRL2, 0x23);
		/* Trimming of VBG reference - 1.2V,
		 * Trimming of boost output voltage - 18.0V
		 */
		regmap_write(sma6201->regmap, SMA6201_93_BOOST_CTRL0, 0x8C);
		/* Trimming of ramp compensation I-gain - 50pF,
		 * Trimming of switching frequency - 3.34MHz
		 * Trimming of ramp compensation - 7.37A / us
		 */
		regmap_write(sma6201->regmap, SMA6201_94_BOOST_CTRL1, 0x9B);
		/* Trimming of over current limit - 3.1A,
		 * Trimming of ramp compensation - P-gain:3.5Mohm,
		 * Type II I-gain:0.7pF
		 */
		regmap_write(sma6201->regmap, SMA6201_95_BOOST_CTRL2, 0x44);
	} else {
		/* Feedback gain trimming - No trimming,
		 * Recovery Current Control Mode - Enhanced mode,
		 * Recovery Current Control Enable,
		 * PWM frequency - 740kHz,
		 * Differential OPAMP bias current - 80uA
		 */
		regmap_write(sma6201->regmap, SMA6201_92_FDPEC_CTRL2, 0x02);
		/* Trimming of VBG reference - 1.2V,
		 * Trimming of boost output voltage - 19.0V
		 */
		regmap_write(sma6201->regmap, SMA6201_93_BOOST_CTRL0, 0x8D);
		/* Trimming of ramp compensation I-gain - 50pF,
		 * Trimming of switching frequency - 3.34MHz
		 * Trimming of ramp compensation - 9.22A / us
		 */
		regmap_write(sma6201->regmap, SMA6201_94_BOOST_CTRL1, 0x9D);
		/* Trimming of over current limit - 3.1A,
		 * Trimming of ramp compensation - P-gain:3.0Mohm,
		 * Type II I-gain:2.0pF
		 */
		regmap_write(sma6201->regmap, SMA6201_95_BOOST_CTRL2, 0x4B);
	}

	/* Trimming of driver deadtime - 10.4ns,
	 * Trimming of boost OCP - pMOS OCP enable, nMOS OCP enable,
	 * Trimming of switching slew - 3ns
	 */
	regmap_write(sma6201->regmap, SMA6201_96_BOOST_CTRL3, 0x3E);

	if (sma6201->rev_num == REV_NUM_REV0) {
		/* Trimming of boost level reference
		 * - 0.825,0.70,0.575,0.50,0.40,0.28
		 * Trimming of minimum on-time - 59ns
		 */
		regmap_write(sma6201->regmap,
			SMA6201_97_BOOST_CTRL4, 0xA4);
	} else {
		/* Trimming of boost level reference
		 * - 0.875,0.700,0.525,0.40,0.32,0.28
		 * Trimming of minimum on-time - 68ns
		 */
		regmap_write(sma6201->regmap,
			SMA6201_97_BOOST_CTRL4, 0x41);
		regmap_write(sma6201->regmap,
			SMA6201_38_DIS_CLASSH_LVL12, 0xC8);
	}

	/* PLL Lock enable, External clock  operation */
	regmap_write(sma6201->regmap, SMA6201_A2_TOP_MAN1, 0x69);
	/* External clock monitoring mode */
	regmap_write(sma6201->regmap, SMA6201_A7_TOP_MAN3, 0x20);

	if (sma6201->rev_num == REV_NUM_REV0) {
		/* Apply -1.0dB fine volume to prevent SPK OCP */
		regmap_write(sma6201->regmap,
			SMA6201_A9_TONE_FINE_VOL, 0x87);
	} else {
		/* Apply -1.25dB fine volume to prevent SPK OCP */
		regmap_write(sma6201->regmap,
			SMA6201_A9_TONE_FINE_VOL, 0x97);
	}
	/* Turn off the tone generator by default */
	regmap_update_bits(sma6201->regmap, SMA6201_A9_TONE_FINE_VOL,
				TONE_VOL_MASK, TONE_VOL_OFF);
	regmap_update_bits(sma6201->regmap, SMA6201_A8_TONE_GENERATOR,
				TONE_ON_MASK, TONE_OFF);

	/* Speaker OCP level - 3.7A */
	regmap_write(sma6201->regmap, SMA6201_AD_SPK_OCP_LVL, 0x46);
	/* High-Z for IRQ pin (IRQ skip mode) */
	regmap_write(sma6201->regmap, SMA6201_AE_TOP_MAN4, 0x40);
	/* VIN sensing Power down, VIN cut off freq - 34kHz,
	 * SAR clock freq - 3.072MHz
	 */
	regmap_write(sma6201->regmap, SMA6201_AF_VIN_SENSING, 0x01);

	/* Brown Out Protection Normal operation */
	regmap_write(sma6201->regmap, SMA6201_B0_BROWN_OUT_P0, 0x85);

	if (sma6201->rev_num == REV_NUM_REV0) {
		/* Class-H Initial Setting */
		regmap_write(sma6201->regmap,
			SMA6201_0D_CLASS_H_CTRL_LVL1, 0x4C);
		regmap_write(sma6201->regmap,
			SMA6201_0F_CLASS_H_CTRL_LVL2, 0x3B);
		regmap_write(sma6201->regmap,
			SMA6201_28_CLASS_H_CTRL_LVL3, 0x5A);
		regmap_write(sma6201->regmap,
			SMA6201_29_CLASS_H_CTRL_LVL4, 0x89);
		regmap_write(sma6201->regmap,
			SMA6201_2A_CLASS_H_CTRL_LVL5, 0x68);
		regmap_write(sma6201->regmap,
			SMA6201_90_CLASS_H_CTRL_LVL6, 0x87);
		regmap_write(sma6201->regmap,
			SMA6201_91_CLASS_H_CTRL_LVL7, 0xB6);
	} else {
		/* Class-H Initial Setting */
		regmap_write(sma6201->regmap,
			SMA6201_0D_CLASS_H_CTRL_LVL1, 0x9C);
		regmap_write(sma6201->regmap,
			SMA6201_0F_CLASS_H_CTRL_LVL2, 0x6B);
		regmap_write(sma6201->regmap,
			SMA6201_28_CLASS_H_CTRL_LVL3, 0x7A);
		regmap_write(sma6201->regmap,
			SMA6201_29_CLASS_H_CTRL_LVL4, 0xA9);
		regmap_write(sma6201->regmap,
			SMA6201_2A_CLASS_H_CTRL_LVL5, 0x68);
		regmap_write(sma6201->regmap,
			SMA6201_90_CLASS_H_CTRL_LVL6, 0x97);
		regmap_write(sma6201->regmap,
			SMA6201_91_CLASS_H_CTRL_LVL7, 0xC6);
		regmap_write(sma6201->regmap,
			SMA6201_38_DIS_CLASSH_LVL12, 0xC8);
	}

	if (sma6201->src_bypass == true) {
		regmap_update_bits(sma6201->regmap, SMA6201_03_INPUT1_CTRL3,
			BP_SRC_MASK, BP_SRC_BYPASS);

		if (sma6201->stereo_two_chip == false)
			regmap_update_bits(sma6201->regmap, SMA6201_A3_TOP_MAN2,
				BP_SRC_MIX_MASK, BP_SRC_MIX_MONO);
		else
			regmap_update_bits(sma6201->regmap, SMA6201_A3_TOP_MAN2,
				BP_SRC_MIX_MASK, BP_SRC_MIX_NORMAL);
	} else {
		regmap_update_bits(sma6201->regmap, SMA6201_03_INPUT1_CTRL3,
			BP_SRC_MASK, BP_SRC_NORMAL);
	}

	if (sma6201->sys_clk_id == SMA6201_EXTERNAL_CLOCK_19_2
		|| sma6201->sys_clk_id == SMA6201_PLL_CLKIN_MCLK) {
		regmap_update_bits(sma6201->regmap, SMA6201_00_SYSTEM_CTRL,
			CLKSYSTEM_MASK, EXT_19_2);

		regmap_update_bits(sma6201->regmap, SMA6201_03_INPUT1_CTRL3,
			BP_SRC_MASK, BP_SRC_NORMAL);
	}

	dev_info(component->dev,
		"%s init_vol is 0x%x\n", __func__, sma6201->init_vol);
	/* EQ1 register value writing
	 * if register value is available from DT
	 */
	if (sma6201->eq1_reg_array != NULL) {
		for (cnt = 0; cnt < eq_len; cnt += 2) {
			reg_val = (struct reg_default *)
				&sma6201->eq1_reg_array[cnt];
			dev_dbg(component->dev, "%s : eq1 reg_write [0x%02x, 0x%02x]",
					__func__, be32_to_cpu(reg_val->reg),
						be32_to_cpu(reg_val->def));
			regmap_write(sma6201->regmap, be32_to_cpu(reg_val->reg),
					be32_to_cpu(reg_val->def));
		}
	}
	/* EQ2 register value writing
	 * if register value is available from DT
	 */
	regmap_update_bits(sma6201->regmap, SMA6201_2B_EQ_MODE,
			EQ_BANK_SEL_MASK, EQ2_BANK_SEL);
	if (sma6201->eq2_reg_array != NULL) {
		for (cnt = 0; cnt < eq_len; cnt += 2) {
			reg_val = (struct reg_default *)
				&sma6201->eq2_reg_array[cnt];
			dev_dbg(component->dev, "%s : eq2 reg_write [0x%02x, 0x%02x]",
					__func__, be32_to_cpu(reg_val->reg),
						be32_to_cpu(reg_val->def));
			regmap_write(sma6201->regmap, be32_to_cpu(reg_val->reg),
					be32_to_cpu(reg_val->def));
		}
	}
	regmap_update_bits(sma6201->regmap, SMA6201_2B_EQ_MODE,
			EQ_BANK_SEL_MASK, EQ1_BANK_SEL);
	/* BrownOut Protection register value writing
	 * if register value is available from DT
	 */
	if (sma6201->bo_reg_array != NULL) {
		for (cnt = 0; cnt < bo_len; cnt += 2) {
			reg_val = (struct reg_default *)
				&sma6201->bo_reg_array[cnt];
			dev_dbg(component->dev, "%s reg_write [0x%02x, 0x%02x]",
					__func__, be32_to_cpu(reg_val->reg),
						be32_to_cpu(reg_val->def));
			regmap_write(sma6201->regmap, be32_to_cpu(reg_val->reg),
					be32_to_cpu(reg_val->def));
		}
	}

	/* Ready to start amp, if need, add amp on/off mix */
	sma6201->voice_music_class_h_mode = SMA6201_CLASS_H_MODE_OFF;
	sma6201->ocp_count = 0;

	return 0;
}

static ssize_t check_thermal_vbat_period_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int rc;

	rc = (int)snprintf(buf, PAGE_SIZE,
			"%ld\n", sma6201->check_thermal_vbat_period);

	return (ssize_t)rc;
}

static ssize_t check_thermal_vbat_period_store(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int ret;

	ret = kstrtol(buf, 10, &sma6201->check_thermal_vbat_period);

	if (ret)
		return -EINVAL;

	return (ssize_t)count;
}

static DEVICE_ATTR_RW(check_thermal_vbat_period);

static ssize_t check_thermal_vbat_enable_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int rc;

	rc = (int)snprintf(buf, PAGE_SIZE,
			"%ld\n", sma6201->check_thermal_vbat_enable);

	return (ssize_t)rc;
}

static ssize_t check_thermal_vbat_enable_store(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int ret;

	ret = kstrtol(buf, 10, &sma6201->check_thermal_vbat_enable);

	if (ret)
		return -EINVAL;

	return (ssize_t)count;
}

static DEVICE_ATTR_RW(check_thermal_vbat_enable);

static ssize_t check_thermal_table_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int i, rc = 0;

	rc += (int)snprintf(buf + rc, PAGE_SIZE,
			"Piezo Thermal Table Summary\n");

	for (i = 0; i < sma6201->num_of_temperature_matches; i++) {
		rc += (int)snprintf(buf+rc,
		PAGE_SIZE, "TEMP[%d] GAIN_C[%d] OCP_N[%d] HIT_N[%d] ACT[%d]\n",
			sma6201->temp_match[i].thermal_limit,
			sma6201->temp_match[i].comp_gain,
			sma6201->temp_match[i].ocp_count,
			sma6201->temp_match[i].hit_count,
			sma6201->temp_match[i].activate);
	}

	return (ssize_t)rc;
}

static DEVICE_ATTR_RO(check_thermal_table);

static ssize_t check_thermal_value_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int ret;
	int i, rc = 0;
	struct outside_status fifo_buf_out = {0, };

	for (i = 0 ; i < FIFO_BUFFER_SIZE ; i++) {
		ret = kfifo_out_peek(&sma6201->data_fifo,
			(unsigned char *)&fifo_buf_out,
			sizeof(fifo_buf_out));

		if (ret != sizeof(fifo_buf_out))
			return -EINVAL;

		rc += (int)snprintf(buf + rc, PAGE_SIZE,
				"%d\n", fifo_buf_out.thermal_deg);
	}

	return (ssize_t)rc;
}

static DEVICE_ATTR_RO(check_thermal_value);

static ssize_t temp_table_number_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int rc;

	rc = (int)snprintf(buf, PAGE_SIZE,
			"%ld\n", sma6201->temp_table_number);

	return (ssize_t)rc;
}

static ssize_t temp_table_number_store(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int ret;

	ret = kstrtol(buf, 10, &sma6201->temp_table_number);

	if (ret || (sma6201->temp_table_number < 0) ||
			(sma6201->temp_table_number >
				ARRAY_SIZE(sma6201_temperature_gain_matches))) {
		sma6201->temp_table_number = 0;
		return -EINVAL;
	}

	return (ssize_t)count;
}

static DEVICE_ATTR_RW(temp_table_number);

static ssize_t temp_limit_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int rc;

	rc = (int)snprintf(buf, PAGE_SIZE, "%d\n",
	sma6201->temp_match[sma6201->temp_table_number].thermal_limit);

	return (ssize_t)rc;
}

static ssize_t temp_limit_store(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int ret;

	ret = kstrtol(buf, 10, &sma6201->temp_limit);

	sma6201->temp_match[sma6201->temp_table_number].thermal_limit =
		(int)sma6201->temp_limit;

	if (ret)
		return -EINVAL;

	return (ssize_t)count;
}

static DEVICE_ATTR_RW(temp_limit);


static ssize_t temp_comp_gain_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int rc;

	rc = (int)snprintf(buf, PAGE_SIZE, "%d\n",
	sma6201->temp_match[sma6201->temp_table_number].comp_gain);

	return (ssize_t)rc;
}

static ssize_t temp_comp_gain_store(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int ret;

	ret = kstrtol(buf, 10, &sma6201->temp_comp_gain);

	if (ret)
		return -EINVAL;

	sma6201->temp_match[sma6201->temp_table_number].comp_gain =
		(int)sma6201->temp_comp_gain;

	return (ssize_t)count;
}

static DEVICE_ATTR_RW(temp_comp_gain);


static ssize_t temp_ocp_count_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int rc;

	rc = (int)snprintf(buf, PAGE_SIZE, "%d\n",
	sma6201->temp_match[sma6201->temp_table_number].ocp_count);

	return (ssize_t)rc;
}

static DEVICE_ATTR_RO(temp_ocp_count);

static ssize_t temp_hit_count_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int rc;

	rc = (int)snprintf(buf, PAGE_SIZE, "%d\n",
	sma6201->temp_match[sma6201->temp_table_number].hit_count);

	return (ssize_t)rc;
}

static DEVICE_ATTR_RO(temp_hit_count);

static ssize_t temp_activate_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int rc;

	rc = (int)snprintf(buf, PAGE_SIZE, "%d\n",
	sma6201->temp_match[sma6201->temp_table_number].activate);

	return (ssize_t)rc;
}

static ssize_t temp_activate_store(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int ret;

	ret = kstrtol(buf, 10, &sma6201->temp_activate);

	if (ret)
		return -EINVAL;

	sma6201->temp_match[sma6201->temp_table_number].activate =
		(unsigned int)sma6201->temp_activate;

	return (ssize_t)count;
}

static DEVICE_ATTR_RW(temp_activate);

static ssize_t enable_ocp_aging_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int rc;

	rc = (int)snprintf(buf, PAGE_SIZE,
			"%ld\n", sma6201->enable_ocp_aging);

	return (ssize_t)rc;
}

static ssize_t enable_ocp_aging_store(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int ret;

	ret = kstrtol(buf, 10, &sma6201->enable_ocp_aging);

	if (ret)
		return -EINVAL;

	return (ssize_t)count;
}

static DEVICE_ATTR_RW(enable_ocp_aging);

static ssize_t check_thermal_fault_period_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int rc;

	rc = (int)snprintf(buf, PAGE_SIZE,
			"%ld\n", sma6201->check_thermal_fault_period);

	return (ssize_t)rc;
}

static ssize_t check_thermal_fault_period_store(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int ret;

	ret = kstrtol(buf, 10, &sma6201->check_thermal_fault_period);

	if (ret)
		return -EINVAL;

	return (ssize_t)count;
}

static DEVICE_ATTR_RW(check_thermal_fault_period);

static ssize_t check_thermal_fault_enable_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int rc;

	rc = (int)snprintf(buf, PAGE_SIZE,
			"%ld\n", sma6201->check_thermal_fault_enable);

	return (ssize_t)rc;
}

static ssize_t check_thermal_fault_enable_store(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int ret;

	ret = kstrtol(buf, 10, &sma6201->check_thermal_fault_enable);

	if (ret)
		return -EINVAL;

	return (ssize_t)count;
}

static DEVICE_ATTR_RW(check_thermal_fault_enable);

static ssize_t check_thermal_sensor_opt_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int rc = 0;

	/*quiet_therm :skin-therm, piezo_therm: wp-therm, vts: vts-virt-therm*/
	if (sma6201->thermal_sense_opt == -1) {
		rc = (int)snprintf(buf, PAGE_SIZE,
				"default selected: skin(1), piezo(2)\n");
	} else {
		switch (sma6201->thermal_sense_opt) {
		case 1:
			rc = (int)snprintf(buf, PAGE_SIZE,
					"quiet_therm(skin-therm) selected\n");
			break;
		case 2:
			rc = (int)snprintf(buf, PAGE_SIZE,
					"piezo_therm(wp-therm) selected\n");
			break;
		}
	}
	return (ssize_t)rc;
}

static ssize_t check_thermal_sensor_opt_store(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int ret;

	ret = kstrtol(buf, 10, &sma6201->thermal_sense_opt);

	if (ret || (sma6201->thermal_sense_opt != 1
			&& sma6201->thermal_sense_opt != 2)){
		sma6201->thermal_sense_opt = 1;
		return -EINVAL;
	}
	return (ssize_t)count;
}

static DEVICE_ATTR_RW(check_thermal_sensor_opt);

static ssize_t delayed_shutdown_enable_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int rc;

	rc = (int)snprintf(buf, PAGE_SIZE,
			"%ld\n", sma6201->delayed_shutdown_enable);

	return (ssize_t)rc;
}

static ssize_t delayed_shutdown_enable_store(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int ret;

	ret = kstrtol(buf, 10, &sma6201->delayed_shutdown_enable);

	if (ret)
		return -EINVAL;

	return (ssize_t)count;
}

static DEVICE_ATTR_RW(delayed_shutdown_enable);

static ssize_t delayed_time_shutdown_show(struct device *dev,
	struct device_attribute *devattr, char *buf)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int rc;

	rc = (int)snprintf(buf, PAGE_SIZE,
			"%ld\n", sma6201->delayed_time_shutdown);

	return (ssize_t)rc;
}

static ssize_t delayed_time_shutdown_store(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct sma6201_priv *sma6201 = dev_get_drvdata(dev);
	int ret;

	ret = kstrtol(buf, 10, &sma6201->delayed_time_shutdown);

	if (ret)
		return -EINVAL;

	return (ssize_t)count;
}

static DEVICE_ATTR_RW(delayed_time_shutdown);

static struct attribute *sma6201_attr[] = {
	&dev_attr_check_thermal_vbat_period.attr,
	&dev_attr_check_thermal_vbat_enable.attr,
	&dev_attr_check_thermal_table.attr,
	&dev_attr_check_thermal_value.attr,
	&dev_attr_temp_table_number.attr,
	&dev_attr_temp_limit.attr,
	&dev_attr_temp_comp_gain.attr,
	&dev_attr_temp_ocp_count.attr,
	&dev_attr_temp_hit_count.attr,
	&dev_attr_temp_activate.attr,
	&dev_attr_enable_ocp_aging.attr,
	&dev_attr_check_thermal_fault_period.attr,
	&dev_attr_check_thermal_fault_enable.attr,
	&dev_attr_check_thermal_sensor_opt.attr,
	&dev_attr_delayed_shutdown_enable.attr,
	&dev_attr_delayed_time_shutdown.attr,
	NULL,
};

static struct attribute_group sma6201_attr_group = {
	.attrs = sma6201_attr,
	.name = "thermal_comp",
};

static int sma6201_probe(struct snd_soc_component *component)
{
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);
	struct snd_soc_dapm_context *dapm =
		snd_soc_component_get_dapm(component);
	char *dapm_widget_str = NULL;
	int prefix_len, ret = 0;
	int str_max = 30;

	dev_info(component->dev, "%s\n", __func__);

	if (component->name_prefix != NULL) {
		dev_info(component->dev, "%s : component name prefix - %s\n",
			__func__, component->name_prefix);

		prefix_len = strlen(component->name_prefix);
		dapm_widget_str = kzalloc(prefix_len + str_max, GFP_KERNEL);

		if (!dapm_widget_str)
			return -ENOMEM;

		strcpy(dapm_widget_str, component->name_prefix);
		strcat(dapm_widget_str, " Playback");

		snd_soc_dapm_ignore_suspend(dapm, dapm_widget_str);

		memset(dapm_widget_str + prefix_len, 0, str_max);

		strcpy(dapm_widget_str, component->name_prefix);
		strcat(dapm_widget_str, " SPK");

		snd_soc_dapm_ignore_suspend(dapm, dapm_widget_str);
	} else {
		snd_soc_dapm_ignore_suspend(dapm, "Playback");
		snd_soc_dapm_ignore_suspend(dapm, "SPK");
	}

	snd_soc_dapm_sync(dapm);

	if (dapm_widget_str != NULL)
		kfree(dapm_widget_str);

	sma6201_reset(component);

	ret = kfifo_alloc(&sma6201->data_fifo,
		sizeof(struct outside_status) * FIFO_BUFFER_SIZE,
		GFP_KERNEL);
	if (ret)
		dev_err(component->dev, "%s: fifo alloc failed\n", __func__);
	else {
		wakeup_source_init(&sma6201->shutdown_wakesrc,
					"shutdown_wakesrc");
	}
	return ret;
}

static void sma6201_remove(struct snd_soc_component *component)
{
	struct sma6201_priv *sma6201 = snd_soc_component_get_drvdata(component);

	dev_info(component->dev, "%s\n", __func__);

	sma6201_set_bias_level(component, SND_SOC_BIAS_OFF);
	devm_free_irq(sma6201->dev, sma6201->irq, sma6201);
	devm_kfree(sma6201->dev, sma6201);

	kfifo_free(&sma6201->data_fifo);
}

static const struct snd_soc_component_driver sma6201_component = {
	.probe = sma6201_probe,
	.remove = sma6201_remove,
	.suspend = sma6201_suspend,
	.resume = sma6201_resume,
	.controls = sma6201_snd_controls,
	.num_controls = ARRAY_SIZE(sma6201_snd_controls),
	.dapm_widgets = sma6201_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sma6201_dapm_widgets),
	.dapm_routes = sma6201_audio_map,
	.num_dapm_routes = ARRAY_SIZE(sma6201_audio_map),
};

const struct regmap_config sma_i2c_regmap = {
	.reg_bits = 8,
	.val_bits = 8,

	.max_register = SMA6201_FF_VERSION,
	.readable_reg = sma6201_readable_register,
	.writeable_reg = sma6201_writeable_register,
	.volatile_reg = sma6201_volatile_register,

	.cache_type = REGCACHE_NONE,
	.reg_defaults = sma6201_reg_def,
	.num_reg_defaults = ARRAY_SIZE(sma6201_reg_def),
};

static int sma6201_i2c_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	struct sma6201_priv *sma6201;
	struct device_node *np = client->dev.of_node;
	int ret;
	u32 value, value_clk;
	unsigned int device_info;

	dev_info(&client->dev, "%s is here. Driver version REV008\n", __func__);

	sma6201 = devm_kzalloc(&client->dev, sizeof(struct sma6201_priv),
							GFP_KERNEL);

	if (!sma6201)
		return -ENOMEM;

	sma6201->regmap = devm_regmap_init_i2c(client, &sma_i2c_regmap);
	if (IS_ERR(sma6201->regmap)) {
		ret = PTR_ERR(sma6201->regmap);
		dev_err(&client->dev,
			"Failed to allocate register map: %d\n", ret);
		return ret;
	}

	if (np) {
		if (!of_property_read_u32(np, "init-vol", &value)) {
			dev_info(&client->dev,
				"init-vol is 0x%x from DT\n", value);
			sma6201->init_vol = value;
		} else {
			dev_info(&client->dev,
				"init-vol is set with 0x30(0dB)\n");
			sma6201->init_vol = 0x30;
		}
		if (of_property_read_bool(np, "stereo-two-chip")) {
			dev_info(&client->dev, "Stereo for two chip solution\n");
				sma6201->stereo_two_chip = true;
		} else {
			dev_info(&client->dev, "Mono for one chip solution\n");
				sma6201->stereo_two_chip = false;
		}
		if (!of_property_read_u32(np, "sys-clk-id", &value)) {
			switch (value) {
			case SMA6201_EXTERNAL_CLOCK_19_2:
				dev_info(&client->dev, "Use the external 19.2MHz clock\n");
				break;
			case SMA6201_EXTERNAL_CLOCK_24_576:
				dev_info(&client->dev, "Use the external 24.576MHz clock\n");
				break;
			case SMA6201_PLL_CLKIN_MCLK:
				if (!of_property_read_u32(np,
					"mclk-freq", &value_clk))
					sma6201->mclk_in = value_clk;
				else
					sma6201->mclk_in = 19200000;

				dev_info(&client->dev,
				"Take an external %dHz clock and covert it to an internal PLL for use\n",
					sma6201->mclk_in);
				break;
			case SMA6201_PLL_CLKIN_BCLK:
				dev_info(&client->dev,
				"Take an BCLK(SCK) and covert it to an internal PLL for use\n");
				break;
			default:
				dev_err(&client->dev,
					"Invalid sys-clk-id: %d\n", value);
				return -EINVAL;
			}
			sma6201->sys_clk_id = value;
		} else {
			dev_info(&client->dev, "Use the internal PLL clock by default\n");
			sma6201->sys_clk_id = SMA6201_PLL_CLKIN_BCLK;
		}
		if (of_property_read_bool(np, "SRC-bypass")) {
			dev_info(&client->dev,
					"Do not set the sample rate converter\n");
				sma6201->src_bypass = true;
		} else {
			dev_info(&client->dev, "Set the sample rate converter\n");
				sma6201->src_bypass = false;
		}

		sma6201->eq1_reg_array = of_get_property(np, "registers-of-eq1",
			&sma6201->eq_reg_array_len);
		if (sma6201->eq1_reg_array == NULL)
			dev_info(&client->dev,
				"There is no EQ1 registers from DT\n");
		sma6201->eq2_reg_array = of_get_property(np, "registers-of-eq2",
			&sma6201->eq_reg_array_len);
		if (sma6201->eq2_reg_array == NULL)
			dev_info(&client->dev,
				"There is no EQ2 registers from DT\n");

		sma6201->bo_reg_array = of_get_property(np, "registers-of-bo",
			&sma6201->bo_reg_array_len);
		if (sma6201->bo_reg_array == NULL)
			dev_info(&client->dev,
				"There is no BrownOut registers from DT\n");

		sma6201->gpio_int = of_get_named_gpio(np,
				"sma6201,gpio-int", 0);
		if (!gpio_is_valid(sma6201->gpio_int)) {
			dev_err(&client->dev,
			"Looking up %s property in node %s failed %d\n",
			"sma6201,gpio-int", client->dev.of_node->full_name,
			sma6201->gpio_int);
		}

		sma6201->gpio_reset = of_get_named_gpio(np,
				"sma6201,gpio-reset", 0);
		if (!gpio_is_valid(sma6201->gpio_reset)) {
			dev_err(&client->dev,
			"Looking up %s property in node %s failed %d\n",
			"sma6201,gpio-reset", client->dev.of_node->full_name,
			sma6201->gpio_reset);
		}
	} else {
		dev_err(&client->dev,
			"device node initialization error\n");
		return -ENODEV;
	}

	INIT_DELAYED_WORK(&sma6201->check_thermal_fault_work,
		sma6201_check_thermal_fault_worker);
	INIT_DELAYED_WORK(&sma6201->check_thermal_vbat_work,
		sma6201_check_thermal_vbat_worker);
	INIT_DELAYED_WORK(&sma6201->delayed_shutdown_work,
		sma6201_delayed_shutdown_worker);

	mutex_init(&sma6201->lock);
	sma6201->check_thermal_vbat_period = CHECK_COMP_PERIOD_TIME;
	sma6201->check_thermal_fault_period = CHECK_FAULT_PERIOD_TIME;
	sma6201->delayed_time_shutdown = DELAYED_SHUTDOWN_TIME;
	sma6201->threshold_level = 0;
	sma6201->enable_ocp_aging = 0;
	sma6201->temp_table_number = 0;
	sma6201->last_rate = 0;
	sma6201->last_width = 0;
	sma6201->last_channel = 0;

	sma6201->devtype = id->driver_data;
	sma6201->dev = &client->dev;
	sma6201->kobj = &client->dev.kobj;
	sma6201->irq = -1;
	sma6201->pll_matches = sma6201_pll_matches;
	sma6201->num_of_pll_matches = ARRAY_SIZE(sma6201_pll_matches);
	sma6201->temp_match = sma6201_temperature_gain_matches;
	sma6201->num_of_temperature_matches =
		ARRAY_SIZE(sma6201_temperature_gain_matches);

	if (gpio_is_valid(sma6201->gpio_int)) {

		dev_info(&client->dev, "%s , i2c client name: %s\n",
			__func__, dev_name(sma6201->dev));

		ret = gpio_request(sma6201->gpio_int, "sma6201-irq");
		if (ret) {
			dev_info(&client->dev, "gpio_request failed\n");
			return ret;
		}

		sma6201->irq = gpio_to_irq(sma6201->gpio_int);

		/* Get SMA6201 IRQ */
		if (sma6201->irq < 0) {
			dev_warn(&client->dev, "interrupt disabled\n");
		} else {
		/* Request system IRQ for SMA6201 */
			ret = request_threaded_irq(sma6201->irq,
				NULL, sma6201_isr, IRQF_ONESHOT |
				IRQF_TRIGGER_FALLING,
				"sma6201", sma6201);
			if (ret < 0) {
				dev_err(&client->dev, "failed to request IRQ(%u) [%d]\n",
						sma6201->irq, ret);
				sma6201->irq = -1;
				i2c_set_clientdata(client, NULL);
				devm_free_irq(&client->dev,
						sma6201->irq, sma6201);
				devm_kfree(&client->dev, sma6201);
				return ret;
			}
			disable_irq((unsigned int)sma6201->irq);
		}
	} else {
		dev_err(&client->dev,
			"interrupt signal input pin is not found\n");
	}

	if (gpio_is_valid(sma6201->gpio_reset)) {
		if (gpio_request(sma6201->gpio_reset, "sma6201-reset") < 0)
			dev_err(&client->dev, "gpio_request failed\n");
		if (gpio_direction_output(sma6201->gpio_reset, 1) < 0)
			dev_err(&client->dev, "gpio_direction_output failed\n");
		gpio_set_value(sma6201->gpio_reset, 1);
	} else {
		dev_err(&client->dev, "reset signal output pin is not found\n");
	}

	atomic_set(&sma6201->irq_enabled, false);
	i2c_set_clientdata(client, sma6201);

	sma6201->force_amp_power_down = false;
	sma6201->amp_power_status = false;
	sma6201->ext_clk_status = false;
	sma6201->check_thermal_vbat_enable = false;
	sma6201->check_thermal_fault_enable = false;
	sma6201->delayed_shutdown_enable = false;
	sma6201->lowbattery_status = -1;
	sma6201->thermal_sense_opt = -1;

	ret = regmap_read(sma6201->regmap, SMA6201_FF_VERSION, &device_info);

	if ((ret != 0) || ((device_info & 0xF8) != DEVICE_ID)) {
		dev_err(&client->dev, "device initialization error (%d 0x%02X)",
				ret, device_info);
		return -ENODEV;
	}
	dev_info(&client->dev, "chip version 0x%02X\n", device_info);

	ret = snd_soc_register_component(&client->dev,
		&sma6201_component, sma6201_dai,
		ARRAY_SIZE(sma6201_dai));

	/* Create sma6201 sysfs attributes */
	sma6201->attr_grp = &sma6201_attr_group;
	ret = sysfs_create_group(sma6201->kobj, sma6201->attr_grp);

	if (ret) {
		pr_err("failed to create attribute group [%d]\n", ret);
		sma6201->attr_grp = NULL;
	}

	return ret;
}

static int sma6201_i2c_remove(struct i2c_client *client)
{
	struct sma6201_priv *sma6201 =
		(struct sma6201_priv *) i2c_get_clientdata(client);

	dev_info(&client->dev, "%s\n", __func__);

	if (sma6201->irq < 0)
		devm_free_irq(&client->dev, sma6201->irq, sma6201);

	if (sma6201) {
		sysfs_remove_group(sma6201->kobj, sma6201->attr_grp);
		devm_kfree(&client->dev, sma6201);
	}

	snd_soc_unregister_component(&client->dev);

	return 0;
}

static const struct i2c_device_id sma6201_i2c_id[] = {
	{"sma6201", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, sma6201_i2c_id);

static const struct of_device_id sma6201_of_match[] = {
	{ .compatible = "irondevice,sma6201", },
	{ }
};
MODULE_DEVICE_TABLE(of, sma6201_of_match);

static struct i2c_driver sma6201_i2c_driver = {
	.driver = {
		.name = "sma6201",
		.of_match_table = sma6201_of_match,
	},
	.probe = sma6201_i2c_probe,
	.remove = sma6201_i2c_remove,
	.id_table = sma6201_i2c_id,
};

static int __init sma6201_init(void)
{
	int ret;

	pr_info("%s : module init\n", __func__);
	ret = i2c_add_driver(&sma6201_i2c_driver);

	if (ret)
		pr_err("Failed to register sma6201 I2C driver: %d\n", ret);

	return ret;
}

static void __exit sma6201_exit(void)
{
	i2c_del_driver(&sma6201_i2c_driver);
}

module_init(sma6201_init);
module_exit(sma6201_exit);

MODULE_DESCRIPTION("ALSA SoC SMA6201 driver");
MODULE_AUTHOR("GH Park, <gyuhwa.park@irondevice.com>");
MODULE_LICENSE("GPL v2");
