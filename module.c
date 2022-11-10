/*
 * Exo Sense Pi kernel module
 *
 *     Copyright (C) 2020-2022 Sfera Labs S.r.l.
 *
 *     For information, visit https://www.sferalabs.cc
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * LICENSE.txt file for more details.
 *
 */

#include "commons/commons.h"
#include "gpio/gpio.h"
#include "wiegand/wiegand.h"
#include "atecc/atecc.h"
#include "sensirion/sht4x/sht4x.h"
#include "sensirion/sgp40/sgp40.h"
#include "sensirion/sgp40_voc_index/sensirion_voc_algorithm.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/kthread.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sort.h>

#define GPIO_LED 22
#define GPIO_BUZZ 27

#define GPIO_PIR 23

#define GPIO_DO1 12

#define GPIO_DI1 16
#define GPIO_DI2 17

#define GPIO_TTL1 4
#define GPIO_TTL2 5

#define THA_READ_INTERVAL_MS 1000
#define THA_DT_MEDIAN_PERIOD_MS 600000
#define THA_DT_MEDIAN_SAMPLES (THA_DT_MEDIAN_PERIOD_MS / THA_READ_INTERVAL_MS)

#define RH_ADJ_MIN_TEMP_OFFSET (-100)
#define RH_ADJ_MAX_TEMP_OFFSET (400)
#define RH_ADJ_FACTOR (1000)

#define PROCFS_MAX_SIZE 1024

#define SND_EVAL_MAX_BANDS 36

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sfera Labs - http://sferalabs.cc");
MODULE_DESCRIPTION("Exo Sense Pi driver module");
MODULE_VERSION("2.7");

static int temp_calib_m = -1000;
module_param( temp_calib_m, int, S_IRUGO);
MODULE_PARM_DESC(temp_calib_m, " Temperature calibration param M");

static int temp_calib_b = -3000;
module_param( temp_calib_b, int, S_IRUGO);
MODULE_PARM_DESC(temp_calib_b, " Temperature calibration param B");

enum snd_time_weighting_mode {
	FAST_WEIGHTING, SLOW_WEIGHTING, IMPULSE_WEIGHTING
};
const char fast_weight_char = 'F';
const char slow_weight_char = 'S';
const char impulse_weight_char = 'I';

enum snd_frequency_weighting_mode {
	A_WEIGHTING, Z_WEIGHTING, C_WEIGHTING
};
const char a_weight_char = 'A';
const char z_weight_char = 'Z';
const char c_weight_char = 'C';

enum snd_frequency_bands_type {
	ONE_THIRD_OCTAVE, ONE_OCTAVE
};
const char one_octave_freq_band_char = '1';
const char one_third_octave_freq_band_char = '3';

char procfs_buffer[PROCFS_MAX_SIZE];
unsigned long procfs_buffer_size = 0;
struct proc_dir_entry *proc_file;
struct proc_dir_entry *proc_folder;

const char procfs_folder_name[] = "exosensepi";
const char procfs_setting_file_name[] = "sound_eval_settings";
const char default_settings[][PROCFS_MAX_SIZE] =
		{
			{
				"version=2.0.0\n"
					"device=exosensepi-mic\n"
					"time="
			},
			{
				"\n"
					"frequency="
			},
			{
				"\n"
					"interval="
			},
			{
				"\n"
					"freq-bands="
			},
			{
				"\n"
					"period-result=/sys/class/exosensepi/sound_eval/leq_period\n"
					"interval-result=/sys/class/exosensepi/sound_eval/leq_interval\n"
					"period-bands-result=/sys/class/exosensepi/sound_eval/leq_period_bands\n"
					"continuous=1\n"
					"interval-only=0\n"
					"quiet=1\n"
					"disable="
			},
			{
				"\n"
					"setting-check-sec=5\n"
			}
		};

static ssize_t procfile_read(struct file *file, char __user *buffer,
		size_t count, loff_t *offset)
{
	if (*offset > 0 || count < PROCFS_MAX_SIZE) /* we have finished to read, return 0 */
		return 0;

	if (copy_to_user(buffer, procfs_buffer, procfs_buffer_size))
		return -EFAULT;

	*offset = procfs_buffer_size;
	return procfs_buffer_size;
}

static ssize_t procfile_write(struct file *file, const char __user *buffer,
		size_t count, loff_t *f_pos) {
	int tlen;
	char *tmp = kzalloc((count + 1), GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;
	if (copy_from_user(tmp, buffer, count)) {
		kfree(tmp);
		return -EFAULT;
	}

	tlen = PROCFS_MAX_SIZE;
	if (count < PROCFS_MAX_SIZE)
		tlen = count;
	memcpy(&procfs_buffer, tmp, tlen);
	procfs_buffer_size = tlen;
	kfree(tmp);
	return tlen;
}

static int procfile_show(struct seq_file *m, void *v) {
	static char *str = NULL;
	seq_printf(m, "%s\n", str);
	return 0;
}

static int procfile_open(struct inode *inode, struct file *file) {
	return single_open(file, procfile_show, NULL);
}

static struct proc_ops proc_fops = {
	.proc_lseek = seq_lseek,
	.proc_open = procfile_open,
	.proc_read = procfile_read,
	.proc_release = single_release,
	.proc_write = procfile_write,
};

struct DeviceAttrBean {
	struct device_attribute devAttr;
	struct GpioBean *gpio;
};

struct DeviceBean {
	char *name;
	struct device *pDevice;
	struct DeviceAttrBean *devAttrBeans;
};

struct soundEvalResult {
	long l_EQ;
	unsigned long long time_epoch_millisec;
};

struct soundEvalBandsResult {
	long l_EQ[SND_EVAL_MAX_BANDS];
	unsigned long long time_epoch_millisec;
};

struct SoundEvalBean {
	unsigned int setting_time_weight;
	unsigned int setting_freq_weight;
	unsigned long setting_interval;
	unsigned int setting_enable_utility;

	struct soundEvalResult period_res;
	struct soundEvalResult interval_res;

	unsigned int setting_freq_bands_type;
	struct soundEvalBandsResult period_bands_res;
};

static struct class *pDeviceClass;

static ssize_t devAttrPirOnCounter_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t devAttrThaTh_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t devAttrThaThv_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t devAttrThaTempOffset_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t devAttrThaTempOffset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t devAttrLm75aU9_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t devAttrLm75aU16_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t devAttrLm75a_show(struct i2c_client *client, struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t opt3001_show(struct device *dev, struct device_attribute *attr,
		char *buf);

static ssize_t devAttrSndEvalPeriodLEQ_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t devAttrSndEvalPeriodLEQ_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t devAttrSndEvalIntervalLEQ_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t devAttrSndEvalIntervalLEQ_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t devAttrSndEvalTimeWeight_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t devAttrSndEvalTimeWeight_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t devAttrSndEvalFreqWeight_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t devAttrSndEvalFreqWeight_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t devAttrSndEvalIntervalSec_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t devAttrSndEvalIntervalSec_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t devAttrSndEvalEnableUtility_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t devAttrSndEvalEnableUtility_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t devAttrSndEvalPeriodBandsLEQ_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t devAttrSndEvalPeriodBandsLEQ_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

static ssize_t devAttrSndEvalFreqBandsType_show(struct device *dev,
		struct device_attribute *attr, char *buf);

static ssize_t devAttrSndEvalFreqBandsType_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);

struct i2c_client *sht40_i2c_client = NULL;
struct i2c_client *sgp40_i2c_client = NULL;
struct i2c_client *lm75aU16_i2c_client = NULL;
struct i2c_client *lm75aU9_i2c_client = NULL;
struct i2c_client *opt3001_i2c_client = NULL;
struct mutex exosensepi_i2c_mutex;
static VocAlgorithmParams voc_algorithm_params;

static struct task_struct *tha_thread;
static volatile uint16_t tha_ready = false;
static volatile int32_t tha_t, tha_rh, tha_dt, tha_tCal, tha_rhCal,
		tha_voc_index;
static volatile uint16_t tha_sraw;
static volatile int tha_temp_offset = 0;
static int32_t tha_dt_median_buff[THA_DT_MEDIAN_SAMPLES];
static int32_t tha_dt_median_sort[THA_DT_MEDIAN_SAMPLES];
static uint16_t tha_dt_idx = 0;

static int32_t rhAdjLookup[] = { 2089, 2074, 2059, 2044, 2029, 2014, 1999, 1984,
	1970, 1955, 1941, 1927, 1912, 1898, 1885, 1871, 1857, 1843, 1830, 1816,
	1803, 1790, 1777, 1764, 1751, 1738, 1725, 1712, 1700, 1687, 1675, 1663,
	1650, 1638, 1626, 1614, 1603, 1591, 1579, 1567, 1556, 1545, 1533, 1522,
	1511, 1500, 1489, 1478, 1467, 1456, 1445, 1435, 1424, 1414, 1403, 1393,
	1383, 1373, 1363, 1353, 1343, 1333, 1323, 1313, 1304, 1294, 1285, 1275,
	1266, 1257, 1247, 1238, 1229, 1220, 1211, 1202, 1193, 1185, 1176, 1167,
	1159, 1150, 1142, 1133, 1125, 1117, 1109, 1101, 1092, 1084, 1076, 1069,
	1061, 1053, 1045, 1038, 1030, 1022, 1015, 1007, 1000, 993, 985, 978, 971,
	964, 957, 950, 943, 936, 929, 922, 915, 909, 902, 895, 889, 882, 876, 869,
	863, 857, 850, 844, 838, 832, 826, 820, 814, 808, 802, 796, 790, 784, 778,
	773, 767, 761, 756, 750, 745, 739, 734, 728, 723, 718, 713, 707, 702, 697,
	692, 687, 682, 677, 672, 667, 662, 657, 652, 647, 643, 638, 633, 629, 624,
	619, 615, 610, 606, 601, 597, 593, 588, 584, 580, 575, 571, 567, 563, 559,
	555, 551, 547, 543, 539, 535, 531, 527, 523, 519, 515, 511, 508, 504, 500,
	497, 493, 489, 486, 482, 479, 475, 472, 468, 465, 461, 458, 455, 451, 448,
	445, 441, 438, 435, 432, 429, 425, 422, 419, 416, 413, 410, 407, 404, 401,
	398, 395, 392, 389, 387, 384, 381, 378, 375, 373, 370, 367, 364, 362, 359,
	356, 354, 351, 349, 346, 344, 341, 339, 336, 334, 331, 329, 326, 324, 322,
	319, 317, 314, 312, 310, 308, 305, 303, 301, 299, 296, 294, 292, 290, 288,
	286, 284, 282, 280, 277, 275, 273, 271, 269, 267, 265, 264, 262, 260, 258,
	256, 254, 252, 250, 248, 247, 245, 243, 241, 239, 238, 236, 234, 232, 231,
	229, 227, 226, 224, 222, 221, 219, 218, 216, 214, 213, 211, 210, 208, 207,
	205, 204, 202, 201, 199, 198, 196, 195, 193, 192, 191, 189, 188, 186, 185,
	184, 182, 181, 180, 178, 177, 176, 174, 173, 172, 171, 169, 168, 167, 166,
	164, 163, 162, 161, 160, 158, 157, 156, 155, 154, 153, 152, 151, 149, 148,
	147, 146, 145, 144, 143, 142, 141, 140, 139, 138, 137, 136, 135, 134, 133,
	132, 131, 130, 129, 128, 127, 126, 125, 124, 123, 122, 122, 121, 120, 119,
	118, 117, 116, 115, 115, 114, 113, 112, 111, 110, 110, 109, 108, 107, 106,
	106, 105, 104, 103, 103, 102, 101, 100, 100, 99, 98, 97, 97, 96, 95, 95, 94,
	93, 93, 92, 91, 91, 90, 89, 89, 88, 87, 87, 86, 85, 85, 84, 83, 83, 82, 82,
	81, 80, 80, 79, 79, 78, 78, 77, 76, 76, 75, 75, 74, 74, 73, 73, 72, 72, 71,
	70, 70, 69, 69, 68, 68, 67, 67, 66, 66, 65, 65, 65, 64, 64, 63, 63, 62, 62,
	61, 61, 60, 60, 59, 59, 59, 58, 58, 57, 57, 56, 56, 56, 55, 55, 54, 54, 54,
	53, 53 };

static struct SoundEvalBean soundEval = {
	.setting_time_weight = 0,
	.setting_freq_weight = 0,
	.setting_interval = 0,
	.setting_enable_utility = 0,

	.period_res.l_EQ = -1.0,
	.period_res.time_epoch_millisec = 0,

	.interval_res.l_EQ = -1.0,
	.interval_res.time_epoch_millisec = 0,

	.setting_freq_bands_type = ONE_THIRD_OCTAVE,
	.period_bands_res.time_epoch_millisec = 0,
	.period_bands_res.l_EQ = { -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0,
		-1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0,
		-1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0,
		-1.0, -1.0, -1.0, -1.0 },
};

enum digInEnum {
	DI1 = 0,
	DI2,
	DI_SIZE,
};

enum ttlEnum {
	TTL1 = 0,
	TTL2,
	TTL_SIZE,
};

static struct GpioBean gpioLed = {
	.name = "exosensepi_led",
	.gpio = GPIO_LED,
	.mode = GPIO_MODE_OUT,
};

static struct GpioBean gpioBuzz = {
	.name = "exosensepi_buzz",
	.gpio = GPIO_BUZZ,
	.mode = GPIO_MODE_OUT,
};

static struct GpioBean gpioDO1 = {
	.name = "exosensepi_do1",
	.gpio = GPIO_DO1,
	.mode = GPIO_MODE_OUT,
};

static struct DebouncedGpioBean gpioPir = {
	.gpio = {
		.name = "exosensepi_pir",
		.gpio = GPIO_PIR,
		.mode = GPIO_MODE_IN,
	},
};

static struct DebouncedGpioBean gpioDI[] = {
	[DI1] = {
		.gpio = {
			.name = "exosensepi_di1",
			.gpio = GPIO_DI1,
			.mode = GPIO_MODE_IN,
		},
	},
	[DI2] = {
		.gpio = {
			.name = "exosensepi_di2",
			.gpio = GPIO_DI2,
			.mode = GPIO_MODE_IN,
		},
	},
};

static struct GpioBean gpioTtl[] = {
	[TTL1] = {
		.name = "exosensepi_ttl1",
		.gpio = GPIO_TTL1,
	},
	[TTL2] = {
		.name = "exosensepi_ttl2",
		.gpio = GPIO_TTL2,
	},
};

static struct WiegandBean w = {
	.d0 = {
		.gpio = &gpioTtl[TTL1],
	},
	.d1 = {
		.gpio = &gpioTtl[TTL2],
	},
};

static struct DeviceAttrBean devAttrBeansLed[] = {
	{
		.devAttr = {
			.attr = {
				.name = "status",
				.mode = 0660,
			},
			.show = devAttrGpio_show,
			.store = devAttrGpio_store,
		},
		.gpio = &gpioLed,
	},

	{
		.devAttr = {
			.attr = {
				.name = "blink",
				.mode = 0220,
			},
			.show = NULL,
			.store = devAttrGpioBlink_store,
		},
		.gpio = &gpioLed,
	},

	{ }
};

static struct DeviceAttrBean devAttrBeansBuzzer[] = {
	{
		.devAttr = {
			.attr = {
				.name = "status",
				.mode = 0660,
			},
			.show = devAttrGpio_show,
			.store = devAttrGpio_store,
		},
		.gpio = &gpioBuzz,
	},

	{
		.devAttr = {
			.attr = {
				.name = "beep",
				.mode = 0220,
			},
			.show = NULL,
			.store = devAttrGpioBlink_store,
		},
		.gpio = &gpioBuzz,
	},

	{ }
};

static struct DeviceAttrBean devAttrBeansDigitalOut[] = {
	{
		.devAttr = {
			.attr = {
				.name = "do1",
				.mode = 0660,
			},
			.show = devAttrGpio_show,
			.store = devAttrGpio_store,
		},
		.gpio = &gpioDO1,
	},

	{ }
};

static struct DeviceAttrBean devAttrBeansDigitalIn[] = {
	{
		.devAttr = {
			.attr = {
				.name = "di1",
				.mode = 0440,
			},
			.show = devAttrGpio_show,
			.store = NULL,
		},
		.gpio = &gpioDI[DI1].gpio,
	},

	{
		.devAttr = {
			.attr = {
				.name = "di2",
				.mode = 0440,
			},
			.show = devAttrGpio_show,
			.store = NULL,
		},
		.gpio = &gpioDI[DI2].gpio,
	},

	{
		.devAttr = {
			.attr = {
				.name = "di1_deb",
				.mode = 0440,
			},
			.show = devAttrGpioDeb_show,
			.store = NULL,
		},
		.gpio = &gpioDI[DI1].gpio,
	},

	{
		.devAttr = {
			.attr = {
				.name = "di2_deb",
				.mode = 0440,
			},
			.show = devAttrGpioDeb_show,
			.store = NULL,
		},
		.gpio = &gpioDI[DI2].gpio,
	},

	{
		.devAttr = {
			.attr = {
				.name = "di1_deb_on_ms",
				.mode = 0660,
			},
			.show = devAttrGpioDebMsOn_show,
			.store = devAttrGpioDebMsOn_store,
		},
		.gpio = &gpioDI[DI1].gpio,
	},

	{
		.devAttr = {
			.attr = {
				.name = "di1_deb_off_ms",
				.mode = 0660,
			},
			.show = devAttrGpioDebMsOff_show,
			.store = devAttrGpioDebMsOff_store,
		},
		.gpio = &gpioDI[DI1].gpio,
	},

	{
		.devAttr = {
			.attr = {
				.name = "di2_deb_on_ms",
				.mode = 0660,
			},
			.show = devAttrGpioDebMsOn_show,
			.store = devAttrGpioDebMsOn_store,
		},
		.gpio = &gpioDI[DI2].gpio,
	},

	{
		.devAttr = {
			.attr = {
				.name = "di2_deb_off_ms",
				.mode = 0660,
			},
			.show = devAttrGpioDebMsOff_show,
			.store = devAttrGpioDebMsOff_store,
		},
		.gpio = &gpioDI[DI2].gpio,
	},

	{
		.devAttr = {
			.attr = {
				.name = "di1_deb_on_cnt",
				.mode = 0440,
			},
			.show = devAttrGpioDebOnCnt_show,
			.store = NULL,
		},
		.gpio = &gpioDI[DI1].gpio,
	},

	{
		.devAttr = {
			.attr = {
				.name = "di1_deb_off_cnt",
				.mode = 0440,
			},
			.show = devAttrGpioDebOffCnt_show,
			.store = NULL,
		},
		.gpio = &gpioDI[DI1].gpio,
	},

	{
		.devAttr = {
			.attr = {
				.name = "di2_deb_on_cnt",
				.mode = 0440,
			},
			.show = devAttrGpioDebOnCnt_show,
			.store = NULL,
		},
		.gpio = &gpioDI[DI2].gpio,
	},

	{
		.devAttr = {
			.attr = {
				.name = "di2_deb_off_cnt",
				.mode = 0440,
			},
			.show = devAttrGpioDebOffCnt_show,
			.store = NULL,
		},
		.gpio = &gpioDI[DI2].gpio,
	},

	{ }
};

static struct DeviceAttrBean devAttrBeansDigitalIO[] = {
	{
		.devAttr = {
			.attr = {
				.name = "ttl1_mode",
				.mode = 0660,
			},
			.show = devAttrGpioMode_show,
			.store = devAttrGpioMode_store,
		},
		.gpio = &gpioTtl[TTL1],
	},

	{
		.devAttr = {
			.attr = {
				.name = "ttl2_mode",
				.mode = 0660,
			},
			.show = devAttrGpioMode_show,
			.store = devAttrGpioMode_store,
		},
		.gpio = &gpioTtl[TTL2],
	},

	{
		.devAttr = {
			.attr = {
				.name = "ttl1",
				.mode = 0660,
			},
			.show = devAttrGpio_show,
			.store = devAttrGpio_store,
		},
		.gpio = &gpioTtl[TTL1],
	},

	{
		.devAttr = {
			.attr = {
				.name = "ttl2",
				.mode = 0660,
			},
			.show = devAttrGpio_show,
			.store = devAttrGpio_store,
		},
		.gpio = &gpioTtl[TTL2],
	},

	{ }
};

static struct DeviceAttrBean devAttrBeansPir[] = {
	{
		.devAttr = {
			.attr = {
				.name = "status",
				.mode = 0440,
			},
			.show = devAttrGpioDeb_show,
			.store = NULL,
		},
		.gpio = &gpioPir.gpio,
	},

	{
		.devAttr = {
			.attr = {
				.name = "cnt",
				.mode = 0660,
			},
			.show = devAttrGpioDebOnCnt_show,
			.store = devAttrPirOnCounter_store,
		},
		.gpio = &gpioPir.gpio,
	},

	{ }
};

static struct DeviceAttrBean devAttrBeansTha[] = {
	{
		.devAttr = {
			.attr = {
				.name = "temp_rh_voc",
				.mode = 0440,
			},
			.show = devAttrThaThv_show,
			.store = NULL,
		},
	},

	{
		.devAttr = {
			.attr = {
				.name = "temp_rh",
				.mode = 0440,
			},
			.show = devAttrThaTh_show,
			.store = NULL,
		},
	},

	{
		.devAttr = {
			.attr = {
				.name = "temp_offset",
				.mode = 0660,
			},
			.show = devAttrThaTempOffset_show,
			.store = devAttrThaTempOffset_store,
		},
	},

	{ }
};

static struct DeviceAttrBean devAttrBeansSysTemp[] = {
	{
		.devAttr = {
			.attr = {
				.name = "t2",
				.mode = 0440,
			},
			.show = devAttrLm75aU9_show,
			.store = NULL,
		},
	},

	{
		.devAttr = {
			.attr = {
				.name = "t1",
				.mode = 0440,
			},
			.show = devAttrLm75aU16_show,
			.store = NULL,
		},
	},

	{ }
};

static struct DeviceAttrBean devAttrBeansLux[] = {
	{
		.devAttr = {
			.attr = {
				.name = "lux",
				.mode = 0440,
			},
			.show = opt3001_show,
			.store = NULL,
		},
	},

	{ }
};

static struct DeviceAttrBean devAttrBeansAtecc[] = {
	{
		.devAttr = {
			.attr = {
				.name = "serial_num",
				.mode = 0440,
			},
			.show = devAttrAteccSerial_show,
			.store = NULL,
		},
	},

	{ }
};

static struct DeviceAttrBean devAttrBeansSound[] = {
	{
		.devAttr = {
			.attr = {
				.name = "leq_period",
				.mode = 0640,
			},
			.show = devAttrSndEvalPeriodLEQ_show,
			.store = devAttrSndEvalPeriodLEQ_store,
		},
	},

	{
		.devAttr = {
			.attr = {
				.name = "leq_interval",
				.mode = 0640,
			},
			.show = devAttrSndEvalIntervalLEQ_show,
			.store = devAttrSndEvalIntervalLEQ_store,
		},
	},

	{
		.devAttr = {
			.attr = {
				.name = "leq_period_bands",
				.mode = 0640,
			},
			.show = devAttrSndEvalPeriodBandsLEQ_show,
			.store = devAttrSndEvalPeriodBandsLEQ_store,
		},
	},

	{
		.devAttr = {
			.attr = {
				.name = "weight_time",
				.mode = 0660,
			},
			.show = devAttrSndEvalTimeWeight_show,
			.store = devAttrSndEvalTimeWeight_store,
		},
	},

	{
		.devAttr = {
			.attr = {
				.name = "weight_freq",
				.mode = 0660,
			},
			.show = devAttrSndEvalFreqWeight_show,
			.store = devAttrSndEvalFreqWeight_store,
		},
	},

	{
		.devAttr = {
			.attr = {
				.name = "weight_freq_bands",
				.mode = 0660,
			},
			.show = devAttrSndEvalFreqBandsType_show,
			.store = devAttrSndEvalFreqBandsType_store,
		},
	},

	{
		.devAttr = {
			.attr = {
				.name = "interval_sec",
				.mode = 0660,
			},
			.show = devAttrSndEvalIntervalSec_show,
			.store = devAttrSndEvalIntervalSec_store,
		},
	},

	{
		.devAttr = {
			.attr = {
				.name = "enabled",
				.mode = 0660,
			},
			.show = devAttrSndEvalEnableUtility_show,
			.store = devAttrSndEvalEnableUtility_store,
		},
	},

	{ }
};

static struct DeviceAttrBean devAttrBeansWiegand[] = {
	{
		.devAttr = {
			.attr = {
				.name = "enabled",
				.mode = 0660,
			},
			.show = devAttrWiegandEnabled_show,
			.store = devAttrWiegandEnabled_store,
		}
	},

	{
		.devAttr = {
			.attr = {
				.name = "data",
				.mode = 0440,
			},
			.show = devAttrWiegandData_show,
			.store = NULL,
		}
	},

	{
		.devAttr = {
			.attr = {
				.name = "noise",
				.mode = 0440,
			},
			.show = devAttrWiegandNoise_show,
			.store = NULL,
		}
	},

	{
		.devAttr = {
			.attr = {
				.name = "pulse_itvl_min",
				.mode = 0660,
			},
			.show = devAttrWiegandPulseIntervalMin_show,
			.store = devAttrWiegandPulseIntervalMin_store,
		}
	},

	{
		.devAttr = {
			.attr = {
				.name = "pulse_itvl_max",
				.mode = 0660,
			},
			.show = devAttrWiegandPulseIntervalMax_show,
			.store = devAttrWiegandPulseIntervalMax_store,
		}
	},

	{
		.devAttr = {
			.attr = {
				.name = "pulse_width_min",
				.mode = 0660,
			},
			.show = devAttrWiegandPulseWidthMin_show,
			.store = devAttrWiegandPulseWidthMin_store,
		}
	},

	{
		.devAttr = {
			.attr = {
				.name = "pulse_width_max",
				.mode = 0660,
			},
			.show = devAttrWiegandPulseWidthMax_show,
			.store = devAttrWiegandPulseWidthMax_store,
		}
	},

	{ }
};

static struct DeviceBean devices[] = {
	{
		.name = "led",
		.devAttrBeans = devAttrBeansLed,
	},

	{
		.name = "buzzer",
		.devAttrBeans = devAttrBeansBuzzer,
	},

	{
		.name = "digital_out",
		.devAttrBeans = devAttrBeansDigitalOut,
	},

	{
		.name = "digital_in",
		.devAttrBeans = devAttrBeansDigitalIn,
	},

	{
		.name = "digital_io",
		.devAttrBeans = devAttrBeansDigitalIO,
	},

	{
		.name = "tha",
		.devAttrBeans = devAttrBeansTha,
	},

	{
		.name = "sys_temp",
		.devAttrBeans = devAttrBeansSysTemp,
	},

	{
		.name = "lux",
		.devAttrBeans = devAttrBeansLux,
	},

	{
		.name = "wiegand",
		.devAttrBeans = devAttrBeansWiegand,
	},

	{
		.name = "pir",
		.devAttrBeans = devAttrBeansPir,
	},

	{
		.name = "sec_elem",
		.devAttrBeans = devAttrBeansAtecc,
	},

	{
		.name = "sound_eval",
		.devAttrBeans = devAttrBeansSound,
	},

	{ }
};

int write_settings_to_proc_buffer(void) {
	char *tmp = kzalloc(PROCFS_MAX_SIZE, GFP_KERNEL);
	if (tmp != NULL) {
		sprintf(tmp, "%s%d%s%d%s%lu%s%d%s%d%s",
				default_settings[0], soundEval.setting_time_weight,
				default_settings[1], soundEval.setting_freq_weight,
				default_settings[2], soundEval.setting_interval,
				default_settings[3], soundEval.setting_freq_bands_type,
				default_settings[4], !soundEval.setting_enable_utility,
				default_settings[5]);
		memcpy(&procfs_buffer, tmp, strlen(tmp));
		procfs_buffer_size = strlen(tmp);
		kfree(tmp);

		return 0;
	} else {
		printk(KERN_ALERT "exosensepi: * | proc setting file write failed\n");
		return -ENOMEM;
	}
}

struct GpioBean* gpioGetBean(struct device *dev, struct device_attribute *attr) {
	struct DeviceAttrBean *dab;
	dab = container_of(attr, struct DeviceAttrBean, devAttr);
	if (dab == NULL) {
		return NULL;
	}
	return dab->gpio;
}

struct WiegandBean* wiegandGetBean(struct device *dev,
		struct device_attribute *attr) {
	return &w;
}

static ssize_t devAttrPirOnCounter_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count) {
	unsigned long val;

	int ret = kstrtoul(buf, 10, &val);
	if (ret < 0) {
		return ret;
	}
	if (val != 0) {
		return -EINVAL;
	}

	gpioPir.onCnt = 0;

	return count;
}

static bool exosensepi_i2c_lock(void) {
	uint8_t i;
	for (i = 0; i < 20; i++) {
		if (mutex_trylock(&exosensepi_i2c_mutex)) {
			return true;
		}
		msleep(1);
	}
	return false;
}

static void exosensepi_i2c_unlock(void) {
	mutex_unlock(&exosensepi_i2c_mutex);
}

struct i2c_client* sensirion_i2c_client_get(uint8_t address) {
	if (sht40_i2c_client != NULL && sht40_i2c_client->addr == address) {
		return sht40_i2c_client;
	}
	if (sgp40_i2c_client != NULL && sgp40_i2c_client->addr == address) {
		return sgp40_i2c_client;
	}
	return NULL;
}

/**
 * Execute one write transaction on the I2C bus, sending a given number of
 * bytes. The bytes in the supplied buffer must be sent to the given address. If
 * the slave device does not acknowledge any of the bytes, an error shall be
 * returned.
 *
 * @param address 7-bit I2C address to write to
 * @param data    pointer to the buffer containing the data to write
 * @param count   number of bytes to read from the buffer and send over I2C
 * @returns 0 on success, error code otherwise
 */
int8_t sensirion_i2c_write(uint8_t address, const uint8_t *data,
		uint16_t count) {
	struct i2c_client *client;

	client = sensirion_i2c_client_get(address);
	if (client == NULL) {
		return -ENODEV;
	}
	if (i2c_master_send(client, data, count) != count) {
		return -EIO;
	}
	return 0;
}

/**
 * Execute one read transaction on the I2C bus, reading a given number of bytes.
 * If the device does not acknowledge the read command, an error shall be
 * returned.
 *
 * @param address 7-bit I2C address to read from
 * @param data    pointer to the buffer where the data is to be stored
 * @param count   number of bytes to read from I2C and store in the buffer
 * @returns 0 on success, error code otherwise
 */
int8_t sensirion_i2c_read(uint8_t address, uint8_t *data, uint16_t count) {
	struct i2c_client *client;

	client = sensirion_i2c_client_get(address);
	if (client == NULL) {
		return -ENODEV;
	}
	if (i2c_master_recv(client, data, count) != count) {
		return -EIO;
	}
	return 0;
}

/**
 * Sleep for a given number of microseconds. The function should delay the
 * execution for at least the given time, but may also sleep longer.
 *
 * Despite the unit, a <10 millisecond precision is sufficient.
 *
 * @param useconds the sleep time in microseconds
 */
void sensirion_sleep_usec(uint32_t useconds) {
	msleep((useconds / 1000) + 1);
}

static int16_t lm75aRead(struct i2c_client *client, int32_t *temp) {
	if (client == NULL) {
		return -ENODEV;
	}

	*temp = i2c_smbus_read_word_data(client, 0);

	if (*temp < 0) {
		return *temp;
	}

	*temp = ((*temp & 0xff) << 8) + ((*temp >> 8) & 0xe0);
	*temp = ((int16_t) * temp) * 100 / 256;

	return 0;
}

static int32_t cmpint32(const void *a, const void *b) {
	return *(int32_t*) a - *(int32_t*) b;
}

static int16_t thaReadCalibrate(int32_t *t, int32_t *rh, int32_t *dt,
		int32_t *tCal, int32_t *rhCal, uint16_t *sraw, int32_t *voc_index) {
	int rhIdx, i;
	int16_t ret;
	int32_t t9, t16, tOff;

	ret = sht4x_measure_blocking_read(t, rh);
	if (ret < 0) {
		return ret;
	}

	ret = lm75aRead(lm75aU9_i2c_client, &t9);
	if (ret < 0) {
		return ret;
	}

	ret = lm75aRead(lm75aU16_i2c_client, &t16);
	if (ret < 0) {
		return ret;
	}

	ret = sgp40_measure_raw_with_rht_blocking_read(*rh, *t, sraw);
	if (ret < 0) {
		return ret;
	}

	VocAlgorithm_process(&voc_algorithm_params, *sraw, voc_index);

	*dt = t16 - t9;
	if (*dt < 0) {
		*dt = 0;
	}

	if (tha_ready) {
		tha_dt_median_buff[tha_dt_idx] = *dt;
	} else {
		for (i = 0; i < THA_DT_MEDIAN_SAMPLES; i++) {
			tha_dt_median_buff[i] = *dt;
		}
	}
	tha_dt_idx = (tha_dt_idx + 1) % THA_DT_MEDIAN_SAMPLES;

	for (i = 0; i < THA_DT_MEDIAN_SAMPLES; i++) {
		tha_dt_median_sort[i] = tha_dt_median_buff[i];
	}
	sort(tha_dt_median_sort, THA_DT_MEDIAN_SAMPLES, sizeof(int32_t), &cmpint32,
			NULL);

	*dt = tha_dt_median_sort[THA_DT_MEDIAN_SAMPLES / 2];

	// t [°C/1000]
	// rh [%/1000]
	// t9,t16,dt [°C/100]
	// tha_temp_offset [°C/100]
	// temp_calib_b [°C/1000]
	// temp_calib_m [1/1000]

	*tCal = (
			(100 * (*t)) // 100 * t [°C/1000] = t [°C/100000]
			+ (temp_calib_m * (*dt)) // temp_calib_m [1/1000] * dt [°C/100] = temp_calib_m * 1000 * dt [°C/100] = temp_calib_m * dt [°C/100000]
					+ (100 * temp_calib_b) // 100 * temp_calib_b [°C/1000] = temp_calib_b [°C/100000]
			);// [°C/100000]
	*tCal = DIV_ROUND_CLOSEST(*tCal, 1000) + tha_temp_offset; // [°C/100]

	*t /= 10; // [°C/100]
	*rh /= 10; // [%/100]

	tOff = (*t) - (*tCal); // [°C/100]
	tOff = DIV_ROUND_CLOSEST(tOff, 10); // [°C/10]

	if (tOff < RH_ADJ_MIN_TEMP_OFFSET) {
		tOff = RH_ADJ_MIN_TEMP_OFFSET;
	} else if (tOff > RH_ADJ_MAX_TEMP_OFFSET - 1) {
		tOff = RH_ADJ_MAX_TEMP_OFFSET - 1;
	}
	rhIdx = tOff - RH_ADJ_MIN_TEMP_OFFSET;
	*rhCal = (*rh) * RH_ADJ_FACTOR / rhAdjLookup[rhIdx];
	if (*rhCal > 10000) {
		*rhCal = 10000;
	} else if (*rhCal < 0) {
		*rhCal = 0;
	}

	return 0;
}

int thaThreadFunction(void *data) {
	int16_t i, ret;
	int32_t t, rh, dt, tCal, rhCal, voc_index;
	uint16_t sraw;

	while (!kthread_should_stop()) {
		if (!exosensepi_i2c_lock()) {
			msleep(100);
			continue;
		}

		for (i = 0; i < 3; i++) {
			ret = thaReadCalibrate(&t, &rh, &dt, &tCal, &rhCal, &sraw,
					&voc_index);
			if (ret == 0) {
				tha_t = t;
				tha_rh = rh;
				tha_dt = dt;
				tha_tCal = tCal;
				tha_rhCal = rhCal;
				tha_voc_index = voc_index;
				tha_sraw = sraw;
				tha_ready = true;
				break;
			}
		}

		exosensepi_i2c_unlock();

		msleep(THA_READ_INTERVAL_MS);
	}

	return 0;
}

static ssize_t devAttrThaTh_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	if (!tha_ready) {
		return -EBUSY;
	}

	return sprintf(buf, "%d %d %d %d %d\n", tha_dt, tha_t, tha_tCal, tha_rh,
			tha_rhCal);
}

static ssize_t devAttrThaThv_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	if (!tha_ready) {
		return -EBUSY;
	}

	return sprintf(buf, "%d %d %d %d %d %d %d\n", tha_dt, tha_t, tha_tCal,
			tha_rh, tha_rhCal, tha_sraw, tha_voc_index);
}

static ssize_t devAttrThaTempOffset_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	return sprintf(buf, "%d\n", tha_temp_offset);
}

static ssize_t devAttrThaTempOffset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count) {
	int ret;
	long val;

	ret = kstrtol(buf, 10, &val);
	if (ret < 0) {
		return ret;
	}

	tha_temp_offset = val;

	return count;
}

static ssize_t devAttrLm75aU9_show(struct device *dev,
		struct device_attribute *attr,
		char *buf) {
	return devAttrLm75a_show(lm75aU9_i2c_client, dev, attr, buf);
}

static ssize_t devAttrLm75aU16_show(struct device *dev,
		struct device_attribute *attr,
		char *buf) {
	return devAttrLm75a_show(lm75aU16_i2c_client, dev, attr, buf);
}

static ssize_t devAttrLm75a_show(struct i2c_client *client, struct device *dev,
		struct device_attribute *attr, char *buf) {
	int16_t res;
	int32_t temp;

	if (!exosensepi_i2c_lock()) {
		return -EBUSY;
	}

	res = lm75aRead(client, &temp);

	exosensepi_i2c_unlock();

	if (res < 0) {
		return res;
	}

	return sprintf(buf, "%d\n", temp);
}

static ssize_t opt3001_show(struct device *dev, struct device_attribute *attr,
		char *buf) {
	int32_t res;
	int16_t man, exp;

	if (opt3001_i2c_client == NULL) {
		return -ENODEV;
	}

	if (!exosensepi_i2c_lock()) {
		return -EBUSY;
	}

	res = i2c_smbus_read_word_data(opt3001_i2c_client, 0);

	exosensepi_i2c_unlock();

	if (res < 0) {
		return res;
	}

	man = ((res & 0xf) << 8) + ((res >> 8) & 0xff);
	exp = (res >> 4) & 0xf;
	res = man * (1 << exp);

	return sprintf(buf, "%d\n", res);
}

static ssize_t devAttrSndEvalPeriodLEQ_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	return sprintf(buf, "%llu %ld\n", soundEval.period_res.time_epoch_millisec,
			soundEval.period_res.l_EQ);
}

static ssize_t devAttrSndEvalPeriodLEQ_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count) {
	int res = sscanf(buf, "%llu %ld", &soundEval.period_res.time_epoch_millisec,
			&soundEval.period_res.l_EQ);
	if (res != 2) {
		return -EINVAL;
	}
	return count;
}

static ssize_t devAttrSndEvalIntervalLEQ_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	return sprintf(buf, "%llu %ld\n",
			soundEval.interval_res.time_epoch_millisec,
			soundEval.interval_res.l_EQ);
}

static ssize_t devAttrSndEvalIntervalLEQ_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count) {
	int res = sscanf(buf, "%llu %ld",
			&soundEval.interval_res.time_epoch_millisec,
			&soundEval.interval_res.l_EQ);
	if (res != 2) {
		return -EINVAL;
	}
	return count;
}

static ssize_t devAttrSndEvalTimeWeight_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	char val;

	switch (soundEval.setting_time_weight)
	{
	case FAST_WEIGHTING:
		val = fast_weight_char;
		break;
	case SLOW_WEIGHTING:
		val = slow_weight_char;
		break;
	case IMPULSE_WEIGHTING:
		val = impulse_weight_char;
		break;
	default:
		soundEval.setting_time_weight = FAST_WEIGHTING;
		val = fast_weight_char;
		break;
	}

	return sprintf(buf, "%c\n", val);
}

static ssize_t devAttrSndEvalTimeWeight_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count) {
	unsigned int ret = soundEval.setting_time_weight;
	if (toUpper(buf[0]) == fast_weight_char) {
		ret = FAST_WEIGHTING;
	} else if (toUpper(buf[0]) == slow_weight_char) {
		ret = SLOW_WEIGHTING;
	} else if (toUpper(buf[0]) == impulse_weight_char) {
		ret = IMPULSE_WEIGHTING;
	} else {
		return -EINVAL;
	}

	if (ret != soundEval.setting_time_weight) {
		unsigned int pre = soundEval.setting_time_weight;
		soundEval.setting_time_weight = ret;

		int error = write_settings_to_proc_buffer();
		if (error != 0) {
			soundEval.setting_time_weight = pre;
			return error;
		}
	}

	return count;
}

static ssize_t devAttrSndEvalFreqWeight_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	char val;

	switch (soundEval.setting_freq_weight)
	{
	case A_WEIGHTING:
		val = a_weight_char;
		break;
	case Z_WEIGHTING:
		val = z_weight_char;
		break;
	case C_WEIGHTING:
		val = c_weight_char;
		break;
	default:
		soundEval.setting_freq_weight = A_WEIGHTING;
		val = a_weight_char;
		break;
	}

	return sprintf(buf, "%c\n", val);
}

static ssize_t devAttrSndEvalFreqWeight_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count) {
	unsigned int ret = soundEval.setting_freq_weight;
	if (toUpper(buf[0]) == a_weight_char) {
		ret = A_WEIGHTING;
	} else if (toUpper(buf[0]) == z_weight_char) {
		ret = Z_WEIGHTING;
	} else if (toUpper(buf[0]) == c_weight_char) {
		ret = C_WEIGHTING;
	} else {
		return -EINVAL;
	}

	if (ret != soundEval.setting_freq_weight) {
		unsigned int pre = soundEval.setting_freq_weight;
		soundEval.setting_freq_weight = ret;

		int error = write_settings_to_proc_buffer();
		if (error != 0) {
			soundEval.setting_freq_weight = pre;
			return error;
		}
	}

	return count;
}

static ssize_t devAttrSndEvalIntervalSec_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	return sprintf(buf, "%lu\n", soundEval.setting_interval);
}

static ssize_t devAttrSndEvalIntervalSec_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count) {
	int ret;
	long val;

	ret = kstrtol(buf, 10, &val);
	if (ret < 0) {
		return ret;
	}

	if (val != soundEval.setting_interval) {
		long pre = soundEval.setting_interval;
		soundEval.setting_interval = val;

		int error = write_settings_to_proc_buffer();
		if (error != 0) {
			soundEval.setting_interval = pre;
			return error;
		}
	}

	return count;
}

static ssize_t devAttrSndEvalEnableUtility_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	return sprintf(buf, "%d\n", soundEval.setting_enable_utility);
}

static ssize_t devAttrSndEvalEnableUtility_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count) {
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 10, &val);
	if (ret < 0) {
		return ret;
	}

	if (val < 0 || val >= 2) {
		return -EINVAL;
	}

	if (val != soundEval.setting_enable_utility) {
		unsigned int pre = soundEval.setting_enable_utility;
		soundEval.setting_enable_utility = val;

		int error = write_settings_to_proc_buffer();
		if (error != 0) {
			soundEval.setting_enable_utility = pre;
			return error;
		}
	}

	return count;
}

static ssize_t devAttrSndEvalPeriodBandsLEQ_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	char res[256];

	if (soundEval.setting_freq_bands_type == ONE_THIRD_OCTAVE) {
		sprintf(res, "%ld %ld %ld %ld %ld %ld %ld %ld %ld %ld "
				"%ld %ld %ld %ld %ld %ld %ld %ld %ld %ld "
				"%ld %ld %ld %ld %ld %ld %ld %ld %ld %ld "
				"%ld %ld %ld %ld %ld %ld",
				soundEval.period_bands_res.l_EQ[0],
				soundEval.period_bands_res.l_EQ[1],
				soundEval.period_bands_res.l_EQ[2],
				soundEval.period_bands_res.l_EQ[3],
				soundEval.period_bands_res.l_EQ[4],
				soundEval.period_bands_res.l_EQ[5],
				soundEval.period_bands_res.l_EQ[6],
				soundEval.period_bands_res.l_EQ[7],
				soundEval.period_bands_res.l_EQ[8],
				soundEval.period_bands_res.l_EQ[9],
				soundEval.period_bands_res.l_EQ[10],
				soundEval.period_bands_res.l_EQ[11],
				soundEval.period_bands_res.l_EQ[12],
				soundEval.period_bands_res.l_EQ[13],
				soundEval.period_bands_res.l_EQ[14],
				soundEval.period_bands_res.l_EQ[15],
				soundEval.period_bands_res.l_EQ[16],
				soundEval.period_bands_res.l_EQ[17],
				soundEval.period_bands_res.l_EQ[18],
				soundEval.period_bands_res.l_EQ[19],
				soundEval.period_bands_res.l_EQ[20],
				soundEval.period_bands_res.l_EQ[21],
				soundEval.period_bands_res.l_EQ[22],
				soundEval.period_bands_res.l_EQ[23],
				soundEval.period_bands_res.l_EQ[24],
				soundEval.period_bands_res.l_EQ[25],
				soundEval.period_bands_res.l_EQ[26],
				soundEval.period_bands_res.l_EQ[27],
				soundEval.period_bands_res.l_EQ[28],
				soundEval.period_bands_res.l_EQ[29],
				soundEval.period_bands_res.l_EQ[30],
				soundEval.period_bands_res.l_EQ[31],
				soundEval.period_bands_res.l_EQ[32],
				soundEval.period_bands_res.l_EQ[33],
				soundEval.period_bands_res.l_EQ[34],
				soundEval.period_bands_res.l_EQ[35]);
	} else if (soundEval.setting_freq_bands_type == ONE_OCTAVE) {
		sprintf(res, "%ld %ld %ld %ld %ld %ld %ld %ld %ld %ld "
				"%ld %ld",
				soundEval.period_bands_res.l_EQ[0],
				soundEval.period_bands_res.l_EQ[1],
				soundEval.period_bands_res.l_EQ[2],
				soundEval.period_bands_res.l_EQ[3],
				soundEval.period_bands_res.l_EQ[4],
				soundEval.period_bands_res.l_EQ[5],
				soundEval.period_bands_res.l_EQ[6],
				soundEval.period_bands_res.l_EQ[7],
				soundEval.period_bands_res.l_EQ[8],
				soundEval.period_bands_res.l_EQ[9],
				soundEval.period_bands_res.l_EQ[10],
				soundEval.period_bands_res.l_EQ[11]);
	}
	return sprintf(buf, "%llu %s\n",
			soundEval.period_bands_res.time_epoch_millisec, res);

}

static ssize_t devAttrSndEvalPeriodBandsLEQ_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count) {
	int res;
	if (soundEval.setting_freq_bands_type == ONE_THIRD_OCTAVE) {
		res = sscanf(buf, "%llu "
				"%ld %ld %ld %ld %ld %ld %ld %ld %ld %ld "
				"%ld %ld %ld %ld %ld %ld %ld %ld %ld %ld "
				"%ld %ld %ld %ld %ld %ld %ld %ld %ld %ld "
				"%ld %ld %ld %ld %ld %ld",
				&soundEval.period_bands_res.time_epoch_millisec,
				&soundEval.period_bands_res.l_EQ[0],
				&soundEval.period_bands_res.l_EQ[1],
				&soundEval.period_bands_res.l_EQ[2],
				&soundEval.period_bands_res.l_EQ[3],
				&soundEval.period_bands_res.l_EQ[4],
				&soundEval.period_bands_res.l_EQ[5],
				&soundEval.period_bands_res.l_EQ[6],
				&soundEval.period_bands_res.l_EQ[7],
				&soundEval.period_bands_res.l_EQ[8],
				&soundEval.period_bands_res.l_EQ[9],
				&soundEval.period_bands_res.l_EQ[10],
				&soundEval.period_bands_res.l_EQ[11],
				&soundEval.period_bands_res.l_EQ[12],
				&soundEval.period_bands_res.l_EQ[13],
				&soundEval.period_bands_res.l_EQ[14],
				&soundEval.period_bands_res.l_EQ[15],
				&soundEval.period_bands_res.l_EQ[16],
				&soundEval.period_bands_res.l_EQ[17],
				&soundEval.period_bands_res.l_EQ[18],
				&soundEval.period_bands_res.l_EQ[19],
				&soundEval.period_bands_res.l_EQ[20],
				&soundEval.period_bands_res.l_EQ[21],
				&soundEval.period_bands_res.l_EQ[22],
				&soundEval.period_bands_res.l_EQ[23],
				&soundEval.period_bands_res.l_EQ[24],
				&soundEval.period_bands_res.l_EQ[25],
				&soundEval.period_bands_res.l_EQ[26],
				&soundEval.period_bands_res.l_EQ[27],
				&soundEval.period_bands_res.l_EQ[28],
				&soundEval.period_bands_res.l_EQ[29],
				&soundEval.period_bands_res.l_EQ[30],
				&soundEval.period_bands_res.l_EQ[31],
				&soundEval.period_bands_res.l_EQ[32],
				&soundEval.period_bands_res.l_EQ[33],
				&soundEval.period_bands_res.l_EQ[34],
				&soundEval.period_bands_res.l_EQ[35]);
		if (res != 37) {
			return -EINVAL;
		}
	} else if (soundEval.setting_freq_bands_type == ONE_OCTAVE) {
		res = sscanf(buf, "%llu "
				"%ld %ld %ld %ld %ld %ld %ld %ld %ld %ld "
				"%ld %ld",
				&soundEval.period_bands_res.time_epoch_millisec,
				&soundEval.period_bands_res.l_EQ[0],
				&soundEval.period_bands_res.l_EQ[1],
				&soundEval.period_bands_res.l_EQ[2],
				&soundEval.period_bands_res.l_EQ[3],
				&soundEval.period_bands_res.l_EQ[4],
				&soundEval.period_bands_res.l_EQ[5],
				&soundEval.period_bands_res.l_EQ[6],
				&soundEval.period_bands_res.l_EQ[7],
				&soundEval.period_bands_res.l_EQ[8],
				&soundEval.period_bands_res.l_EQ[9],
				&soundEval.period_bands_res.l_EQ[10],
				&soundEval.period_bands_res.l_EQ[11]);
		if (res != 13) {
			return -EINVAL;
		}
	}
	return count;
}

static ssize_t devAttrSndEvalFreqBandsType_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	char val;

	switch (soundEval.setting_freq_bands_type)
	{
	case ONE_THIRD_OCTAVE:
		val = one_third_octave_freq_band_char;
		break;
	case ONE_OCTAVE:
		val = one_octave_freq_band_char;
		break;
	default:
		soundEval.setting_freq_bands_type = ONE_THIRD_OCTAVE;
		val = one_third_octave_freq_band_char;
		break;
	}

	return sprintf(buf, "%c\n", val);
}

static ssize_t devAttrSndEvalFreqBandsType_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count) {
	unsigned int ret = soundEval.setting_freq_bands_type;
	if (toUpper(buf[0]) == one_third_octave_freq_band_char) {
		ret = ONE_THIRD_OCTAVE;
	} else if (toUpper(buf[0]) == one_octave_freq_band_char) {
		ret = ONE_OCTAVE;
	} else {
		return -EINVAL;
	}

	if (ret != soundEval.setting_freq_bands_type) {
		unsigned int pre = soundEval.setting_freq_bands_type;
		soundEval.setting_freq_bands_type = ret;

		int error = write_settings_to_proc_buffer();
		if (error != 0) {
			soundEval.setting_freq_bands_type = pre;
			return error;
		}
	}

	return count;
}

static int exosensepi_i2c_probe(struct i2c_client *client,
		const struct i2c_device_id *id) {
	uint16_t conf;
	int i;
	if (client->addr == 0x44) {
		sht40_i2c_client = client;
	} else if (client->addr == 0x59) {
		sgp40_i2c_client = client;
	} else if (client->addr == 0x48) {
		lm75aU9_i2c_client = client;
	} else if (client->addr == 0x49) {
		lm75aU16_i2c_client = client;
	} else if (client->addr == 0x45) {
		opt3001_i2c_client = client;
		// Setting configuration register to 0xCC10 (default = 0xC810):
		// all default values but M[1:0], set to 10b = continuous conversions
		// M[1:0] defaults to 00b = shutdown
		// bytes order inverted in i2c_smbus_write_word_data()
		conf = 0x10cc;
		for (i = 0; i < 3; i++) {
			if (!i2c_smbus_write_word_data(opt3001_i2c_client, 1, conf)) {
				break;
			}
		}
	}
	printk(KERN_INFO "exosensepi: - | i2c probe addr 0x%02hx\n", client->addr);
	return 0;
}

static int exosensepi_i2c_remove(struct i2c_client *client) {
	printk(KERN_INFO "exosensepi: - | i2c remove addr 0x%02hx\n", client->addr);
	return 0;
}

const struct of_device_id exosensepi_of_match[] = {
	{ .compatible = "sferalabs,exosensepi", },
	{ },
};
MODULE_DEVICE_TABLE( of, exosensepi_of_match);

static const struct i2c_device_id exosensepi_i2c_id[] = {
	{ "exosensepi", 0 },
	{ },
};
MODULE_DEVICE_TABLE( i2c, exosensepi_i2c_id);

static struct i2c_driver exosensepi_i2c_driver = {
	.driver = {
		.name = "exosensepi",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(exosensepi_of_match),
	},
	.probe = exosensepi_i2c_probe,
	.remove = exosensepi_i2c_remove,
	.id_table = exosensepi_i2c_id,
};

static void cleanup(void) {
	struct DeviceBean *db;
	struct DeviceAttrBean *dab;
	int i, di, ai;

	if (tha_thread) {
		kthread_stop(tha_thread);
	}

	i2c_del_driver(&exosensepi_i2c_driver);
	mutex_destroy(&exosensepi_i2c_mutex);

	di = 0;
	while (devices[di].name != NULL) {
		if (devices[di].pDevice && !IS_ERR(devices[di].pDevice)) {
			db = &devices[di];
			ai = 0;
			while (db->devAttrBeans[ai].devAttr.attr.name != NULL) {
				dab = &db->devAttrBeans[ai];
				device_remove_file(db->pDevice, &dab->devAttr);
				ai++;
			}
		}
		device_destroy(pDeviceClass, 0);
		di++;
	}

	if (!IS_ERR(pDeviceClass)) {
		class_destroy(pDeviceClass);
	}

	wiegandDisable(&w);

	if (proc_folder != NULL) {
		if (proc_file != NULL) {
			remove_proc_entry(procfs_setting_file_name, proc_folder);
		}
		remove_proc_entry(procfs_folder_name, NULL);
	}

	for (i = 0; i < DI_SIZE; i++) {
		gpioFreeDebounce(&gpioDI[i]);
	}
	for (i = 0; i < TTL_SIZE; i++) {
		gpioFree(&gpioTtl[i]);
	}
	gpioFree(&gpioLed);
	gpioFree(&gpioBuzz);
	gpioFree(&gpioDO1);
	gpioFreeDebounce(&gpioPir);
}

static int __init exosensepi_init(void) {
	struct DeviceBean *db;
	struct DeviceAttrBean *dab;
	int i, di, ai;

	printk(KERN_INFO "exosensepi: - | init\n");

	i2c_add_driver(&exosensepi_i2c_driver);
	mutex_init(&exosensepi_i2c_mutex);

	ateccAddDriver();

	VocAlgorithm_init(&voc_algorithm_params);

	for (i = 0; i < DI_SIZE; i++) {
		if (gpioInitDebounce(&gpioDI[i])) {
			pr_alert("exosensepi: * | error setting up GPIO %d\n",
					gpioDI[i].gpio.gpio);
			goto fail;
		}
	}
	if (gpioInit(&gpioLed)) {
		pr_alert("exosensepi: * | error setting up GPIO %d\n", gpioLed.gpio);
		goto fail;
	}
	if (gpioInit(&gpioBuzz)) {
		pr_alert("exosensepi: * | error setting up GPIO %d\n", gpioBuzz.gpio);
		goto fail;
	}
	if (gpioInit(&gpioDO1)) {
		pr_alert("exosensepi: * | error setting up GPIO %d\n", gpioDO1.gpio);
		goto fail;
	}
	if (gpioInitDebounce(&gpioPir)) {
		pr_alert("exosensepi: * | error setting up GPIO %d\n",
				gpioPir.gpio.gpio);
		goto fail;
	}
	gpioPir.onMinTime_usec = 0;
	gpioPir.offMinTime_usec = 0;

	wiegandInit(&w);

	pDeviceClass = class_create(THIS_MODULE, "exosensepi");
	if (IS_ERR(pDeviceClass)) {
		printk(KERN_ALERT "exosensepi: * | failed to create device class\n");
		goto fail;
	}

	di = 0;
	while (devices[di].name != NULL) {
		db = &devices[di];
		db->pDevice = device_create(pDeviceClass, NULL, 0, NULL, db->name);
		if (IS_ERR(db->pDevice)) {
			pr_alert("ionopi: * | failed to create device '%s'\n", db->name);
			goto fail;
		}

		ai = 0;
		while (db->devAttrBeans[ai].devAttr.attr.name != NULL) {
			dab = &db->devAttrBeans[ai];
			if (device_create_file(db->pDevice, &dab->devAttr)) {
				pr_alert("ionopi: * | failed to create device file '%s/%s'\n",
						db->name, dab->devAttr.attr.name);
				goto fail;
			}
			ai++;
		}
		di++;
	}

	proc_folder = proc_mkdir(procfs_folder_name, NULL);
	if (NULL == proc_folder) {
		goto fail;
	}

	proc_file = proc_create(procfs_setting_file_name, 0444, proc_folder,
			&proc_fops);
	if (NULL == proc_file) {
		goto fail;
	}

	if (write_settings_to_proc_buffer() != 0) {
		goto fail;
	}

	tha_thread = kthread_run(thaThreadFunction, NULL, "exosensepi THA");
	if (!tha_thread) {
		printk(KERN_ALERT "exosensepi: * | THA thread creation failed\n");
		goto fail;
	}

	printk(KERN_INFO "exosensepi: - | ready\n");
	return 0;

	fail:
	printk(KERN_ALERT "exosensepi: * | init failed\n");
	cleanup();
	return -1;
}

static void __exit exosensepi_exit(void) {
	cleanup();
	printk(KERN_INFO "exosensepi: - | exit\n");
}

module_init( exosensepi_init);
module_exit( exosensepi_exit);
