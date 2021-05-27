/*
 * Exo Sense Pi kernel module
 *
 *     Copyright (C) 2020-2021 Sfera Labs S.r.l.
 *
 *     For information, visit https://www.sferalabs.cc
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * LICENSE.txt file for more details.
 *
 */

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include "sensirion/sht4x/sht4x.h"
#include "sensirion/sgp40/sgp40.h"
#include "sensirion/sgp40_voc_index/sensirion_voc_algorithm.h"

#define GPIO_MODE_IN 1
#define GPIO_MODE_OUT 2

#define GPIO_LED 22
#define GPIO_BUZZ 27

#define GPIO_PIR 23

#define GPIO_DO1 12

#define GPIO_DI1 16
#define GPIO_DI2 17

#define GPIO_TTL1 4
#define GPIO_TTL2 5

#define WIEGAND_MAX_BITS 64

#define RH_ADJ_MIN_TEMP_OFFSET (-100)
#define RH_ADJ_MAX_TEMP_OFFSET (400)
#define RH_ADJ_FACTOR (1000)

#define DEBOUNCE_DEFAULT_TIME_USEC 50000ul
#define DEBOUNCE_STATE_NOT_DEFINED -1

#define DEBOUNCE_STATE_TYPE_ON 1
#define DEBOUNCE_STATE_TYPE_OFF 2

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sfera Labs - http://sferalabs.cc");
MODULE_DESCRIPTION("Exo Sense Pi driver module");
MODULE_VERSION("1.0");

static int temp_calib_m = -1000;
module_param(temp_calib_m, int, S_IRUGO);
MODULE_PARM_DESC(temp_calib_m, " Temperature calibration param M");

static int temp_calib_b = -3000;
module_param(temp_calib_b, int, S_IRUGO);
MODULE_PARM_DESC(temp_calib_b, " Temperature calibration param B");

struct DebounceBean {
	int gpio;
	const char* debIrqDevName;
	int debValue;
	int debPastValue;
	int debIrqNum;
	struct timespec64 lastDebIrqTs;
	unsigned long debOnMinTime_usec;
	unsigned long debOffMinTime_usec;
	unsigned long debOnStateCnt;
	unsigned long debOffStateCnt;
};

struct DeviceAttrBean {
	struct device_attribute devAttr;
	int gpioMode;
	int gpio;
	bool invert;
	struct DebounceBean *debBean;
};

struct DeviceBean {
	char *name;
	struct device *pDevice;
	struct DeviceAttrBean *devAttrBeans;
};

struct WiegandLine {
	int gpio;
	unsigned int irq;
	bool irqRequested;
	bool wasLow;
};

struct WiegandBean {
	struct WiegandLine d0;
	struct WiegandLine d1;
	struct WiegandLine *activeLine;
	unsigned long pulseIntervalMin_usec;
	unsigned long pulseIntervalMax_usec;
	unsigned long pulseWidthMin_usec;
	unsigned long pulseWidthMax_usec;
	bool enabled;
	uint64_t data;
	int bitCount;
	int noise;
	struct timespec64 lastBitTs;
};

static struct class *pDeviceClass;

static ssize_t devAttrGpio_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrGpio_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count);

static ssize_t devAttrGpioDeb_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrGpioDebMsOn_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrGpioDebMsOn_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count);

static ssize_t devAttrGpioDebMsOff_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrGpioDebMsOff_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count);

static ssize_t devAttrGpioDebOnCnt_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrGpioDebOffCnt_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrGpioBlink_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count);

static ssize_t devAttrTtlMode_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrTtlMode_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count);

static ssize_t devAttrThaTh_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrThaThv_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrThaTempOffset_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrThaTempOffset_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count);

static ssize_t devAttrLm75aU9_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrLm75aU16_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrLm75a_show(struct i2c_client *client, struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t opt3001_show(struct device* dev, struct device_attribute* attr,
		char *buf);

static ssize_t devAttrWiegandEnabled_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrWiegandEnabled_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count);

static ssize_t devAttrWiegandData_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrWiegandNoise_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrWiegandPulseIntervalMin_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrWiegandPulseIntervalMin_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count);

static ssize_t devAttrWiegandPulseIntervalMax_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrWiegandPulseIntervalMax_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count);

static ssize_t devAttrWiegandPulseWidthMin_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrWiegandPulseWidthMin_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count);

static ssize_t devAttrWiegandPulseWidthMax_show(struct device* dev,
		struct device_attribute* attr, char *buf);

static ssize_t devAttrWiegandPulseWidthMax_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count);

static unsigned long to_usec(struct timespec64 *t);
static unsigned long diff_usec(struct timespec64 *t1, struct timespec64 *t2);

struct i2c_client *sht40_i2c_client = NULL;
struct i2c_client *sgp40_i2c_client = NULL;
struct i2c_client *lm75aU16_i2c_client = NULL;
struct i2c_client *lm75aU9_i2c_client = NULL;
struct i2c_client *opt3001_i2c_client = NULL;
struct mutex exosensepi_i2c_mutex;
static VocAlgorithmParams voc_algorithm_params;

static bool ttl1enabled = false;
static bool ttl2enabled = false;

static int temp_offset = 0;

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

static struct WiegandBean w1 = {
	.d0 = {
		.gpio = GPIO_TTL1,
		.irqRequested = false,
	},
	.d1 = {
		.gpio = GPIO_TTL2,
		.irqRequested = false,
	},
	.enabled = false,
	.pulseWidthMin_usec = 10,
	.pulseWidthMax_usec = 150,
	.pulseIntervalMin_usec = 1200,
	.pulseIntervalMax_usec = 2700,
	.noise = 0,
};

enum digital_in {
    DI1 = 0,
    DI2,
};

static struct DebounceBean debounceBeans[] ={
	[DI1] = {
		.gpio = GPIO_DI1,
		.debIrqDevName = "exosensepi_di1_deb",
		.debOnMinTime_usec = DEBOUNCE_DEFAULT_TIME_USEC,
		.debOffMinTime_usec = DEBOUNCE_DEFAULT_TIME_USEC,
		.debOnStateCnt = 0,
		.debOffStateCnt = 0,
	},

	[DI2] = {
		.gpio = GPIO_DI2,
		.debIrqDevName = "exosensepi_di2_deb",
		.debOnMinTime_usec = DEBOUNCE_DEFAULT_TIME_USEC,
		.debOffMinTime_usec = DEBOUNCE_DEFAULT_TIME_USEC,
		.debOnStateCnt = 0,
		.debOffStateCnt = 0,
	},

	{ }
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
		.gpioMode = GPIO_MODE_OUT,
		.gpio = GPIO_LED,
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
		.gpioMode = GPIO_MODE_OUT,
		.gpio = GPIO_LED,
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
		.gpioMode = GPIO_MODE_OUT,
		.gpio = GPIO_BUZZ,
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
		.gpioMode = GPIO_MODE_OUT,
		.gpio = GPIO_BUZZ,
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
		.gpioMode = GPIO_MODE_OUT,
		.gpio = GPIO_DO1,
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
		.gpioMode = GPIO_MODE_IN,
		.gpio = GPIO_DI1,
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
		.gpioMode = GPIO_MODE_IN,
		.gpio = GPIO_DI2,
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
		.debBean = &debounceBeans[DI1],
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
		.debBean = &debounceBeans[DI2],
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
		.debBean = &debounceBeans[DI1],
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
		.debBean = &debounceBeans[DI1],
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
		.debBean = &debounceBeans[DI2],
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
		.debBean = &debounceBeans[DI2],
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
		.debBean = &debounceBeans[DI1],
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
		.debBean = &debounceBeans[DI1],
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
		.debBean = &debounceBeans[DI2],
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
		.debBean = &debounceBeans[DI2],
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
			.show = devAttrTtlMode_show,
			.store = devAttrTtlMode_store,
		},
		.gpio = GPIO_TTL1,
	},

	{
		.devAttr = {
			.attr = {
				.name = "ttl2_mode",
				.mode = 0660,
			},
			.show = devAttrTtlMode_show,
			.store = devAttrTtlMode_store,
		},
		.gpio = GPIO_TTL2,
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
		.gpioMode = 0,
		.gpio = GPIO_TTL1,
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
		.gpioMode = 0,
		.gpio = GPIO_TTL2,
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
			.show = devAttrGpio_show,
			.store = NULL,
		},
		.gpioMode = GPIO_MODE_IN,
		.gpio = GPIO_PIR,
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

	{ }
};

static char toUpper(char c) {
	if (c >= 97 && c <= 122) {
		return c - 32;
	}
	return c;
}

static int gpioSetup(struct DeviceBean* db, struct DeviceAttrBean* dab) {
	int result = 0;
	char gpioReqName[128];
	char *gpioReqNamePart;

	strcpy(gpioReqName, "exosensepi_");
	gpioReqNamePart = gpioReqName + strlen("exosensepi_");

	strcpy(gpioReqNamePart, db->name);
	gpioReqNamePart[strlen(db->name)] = '_';

	strcpy(gpioReqNamePart + strlen(db->name) + 1, dab->devAttr.attr.name);

	gpio_request(dab->gpio, gpioReqName);
	if (dab->gpioMode == GPIO_MODE_OUT) {
		result = gpio_direction_output(dab->gpio, false);
	} else if (dab->gpioMode == GPIO_MODE_IN) {
		result = gpio_direction_input(dab->gpio);
	}

	return result;
}

static struct DeviceBean* devGetBean(struct device* dev) {
	int di;
	di = 0;
	while (devices[di].name != NULL) {
		if (dev == devices[di].pDevice) {
			return &devices[di];
		}
		di++;
	}
	return NULL;
}

static struct DeviceAttrBean* devAttrGetBean(struct DeviceBean* devBean,
		struct device_attribute* attr) {
	int ai;
	if (devBean == NULL) {
		return NULL;
	}
	ai = 0;
	while (devBean->devAttrBeans[ai].devAttr.attr.name != NULL) {
		if (attr == &devBean->devAttrBeans[ai].devAttr) {
			return &devBean->devAttrBeans[ai];
		}
		ai++;
	}
	return NULL;
}

static ssize_t devAttrGpio_show(struct device* dev,
		struct device_attribute* attr, char *buf) {
	int val;
	struct DeviceAttrBean* dab;
	dab = devAttrGetBean(devGetBean(dev), attr);
	if (dab == NULL || dab->gpio < 0) {
		return -EFAULT;
	}
	if (dab->gpioMode != GPIO_MODE_IN && dab->gpioMode != GPIO_MODE_OUT) {
		return -EPERM;
	}
	val = gpio_get_value(dab->gpio);
	if (dab->invert) {
		val = val == 0 ? 1 : 0;
	}
	return sprintf(buf, "%d\n", val);
}

static ssize_t devAttrGpio_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count) {
	bool val;
	struct DeviceAttrBean* dab;
	dab = devAttrGetBean(devGetBean(dev), attr);
	if (dab == NULL || dab->gpio < 0) {
		return -EFAULT;
	}
	if (dab->gpioMode != GPIO_MODE_OUT) {
		return -EPERM;
	}
	if (kstrtobool(buf, &val) < 0) {
		if (toUpper(buf[0]) == 'E') { // Enable
			val = true;
		} else if (toUpper(buf[0]) == 'D') { // Disable
			val = false;
		} else if (toUpper(buf[0]) == 'F' || toUpper(buf[0]) == 'T') { // Flip/Toggle
			val = gpio_get_value(dab->gpio) ==
					(dab->invert ? 0 : 1) ? false : true;
		} else {
			return -EINVAL;
		}
	}
	if (dab->invert) {
		val = !val;
	}
	gpio_set_value(dab->gpio, val ? 1 : 0);
	return count;
}

static ssize_t devAttrGpioDeb_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	struct timespec64 now;
	unsigned long diff;
	int actualGPIOStatus;
	struct DeviceAttrBean *dab;

	ktime_get_raw_ts64(&now);
	dab = container_of(attr, struct DeviceAttrBean, devAttr);
	diff = diff_usec((struct timespec64*) &dab->debBean->lastDebIrqTs,
			&now);
	actualGPIOStatus = gpio_get_value(dab->debBean->gpio);
	if (actualGPIOStatus) {
		if (diff >= dab->debBean->debOnMinTime_usec) {
			printk("BCDebug:\t - time diff = %lu usec, actual value = %d\n",
					diff, actualGPIOStatus);
			return sprintf(buf, "%d\n", actualGPIOStatus);
		} else {
			printk("BCDebug:\t - time diff = %lu usec, actual value = %d\n",
					diff, dab->debBean->debValue);
			return sprintf(buf, "%d\n", dab->debBean->debValue);
		}
	} else {
		if (diff >= dab->debBean->debOffMinTime_usec) {
			printk("BCDebug:\t - time diff = %lu usec, actual value = %d\n",
					diff, actualGPIOStatus);
			return sprintf(buf, "%d\n", actualGPIOStatus);
		} else {
			printk("BCDebug:\t - time diff = %lu usec, actual value = %d\n",
					diff, dab->debBean->debValue);
			return sprintf(buf, "%d\n", dab->debBean->debValue);
		}
	}
	return sprintf(buf, "%d\n", dab->debBean->debValue);
}

static ssize_t devAttrGpioDebMsOn_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	struct DeviceAttrBean* dab = container_of(attr, struct DeviceAttrBean, devAttr);

	return sprintf(buf, "%lu\n",
			dab->debBean->debOnMinTime_usec / 1000);
}

static ssize_t devAttrGpioDebMsOff_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	struct DeviceAttrBean* dab = container_of(attr, struct DeviceAttrBean, devAttr);

	return sprintf(buf, "%lu\n",
			dab->debBean->debOffMinTime_usec / 1000);
}

static ssize_t devAttrGpioDebMsOn_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count) {
	struct DeviceAttrBean* dab = container_of(attr, struct DeviceAttrBean, devAttr);
	unsigned int val;

	int ret = kstrtouint(buf, 10, &val);
	if (ret < 0) {
		return ret;
	}
	dab->debBean->debOnMinTime_usec = val * 1000;
	dab->debBean->debOnStateCnt = 0;
	return count;
}

static ssize_t devAttrGpioDebMsOff_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count) {
	struct DeviceAttrBean* dab = container_of(attr, struct DeviceAttrBean, devAttr);
	unsigned int val;

	int ret = kstrtouint(buf, 10, &val);
	if (ret < 0) {
		return ret;
	}
	dab->debBean->debOffMinTime_usec = val * 1000;
	dab->debBean->debOffStateCnt = 0;
	return count;
}

static ssize_t devAttrGpioDebOnCnt_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	struct DeviceAttrBean* dab = container_of(attr, struct DeviceAttrBean, devAttr);
	struct timespec64 now;
	unsigned long diff;
	int actualGPIOStatus;

	ktime_get_raw_ts64(&now);
	diff = diff_usec(
			(struct timespec64*) &dab->debBean->lastDebIrqTs, &now);

	actualGPIOStatus = gpio_get_value(dab->debBean->gpio);
	if (dab->debBean->debPastValue == actualGPIOStatus && actualGPIOStatus
			&& diff >= dab->debBean->debOnMinTime_usec) {
		return sprintf(buf, "%lu\n", dab->debBean->debOnStateCnt + 1);
	}

	return sprintf(buf, "%lu\n", dab->debBean->debOnStateCnt);
}

static ssize_t devAttrGpioDebOffCnt_show(struct device *dev,
		struct device_attribute *attr, char *buf) {
	struct DeviceAttrBean* dab = container_of(attr, struct DeviceAttrBean, devAttr);
	struct timespec64 now;
	unsigned long diff;
	int actualGPIOStatus;

	ktime_get_raw_ts64(&now);
	diff = diff_usec(
			(struct timespec64*) &dab->debBean->lastDebIrqTs, &now);

	actualGPIOStatus = gpio_get_value(dab->debBean->gpio);
	if (dab->debBean->debPastValue == actualGPIOStatus && !actualGPIOStatus
			&& diff >= dab->debBean->debOffMinTime_usec) {
		return sprintf(buf, "%lu\n", dab->debBean->debOffStateCnt + 1);
	}

	return sprintf(buf, "%lu\n", dab->debBean->debOffStateCnt);
}

static ssize_t devAttrGpioBlink_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count) {
	int i;
	struct DeviceAttrBean* dab;
	long on = 0;
	long off = 0;
	long rep = 1;
	char *end = NULL;
	dab = devAttrGetBean(devGetBean(dev), attr);
	if (dab == NULL || dab->gpioMode == 0) {
		return -EFAULT;
	}
	if (dab->gpio < 0) {
		return -EFAULT;
	}
	on = simple_strtol(buf, &end, 10);
	if (++end < buf + count) {
		off = simple_strtol(end, &end, 10);
		if (++end < buf + count) {
			rep = simple_strtol(end, NULL, 10);
		}
	}
	if (rep < 1) {
		rep = 1;
	}
	if (on > 0) {
		for (i = 0; i < rep; i++) {
			gpio_set_value(dab->gpio, dab->invert ? 0 : 1);
			msleep(on);
			gpio_set_value(dab->gpio, dab->invert ? 1 : 0);
			if (i < rep - 1) {
				msleep(off);
			}
		}
	}
	return count;
}

static ssize_t devAttrTtlMode_show(struct device* dev,
		struct device_attribute* attr, char *buf) {
	struct DeviceAttrBean* dab;
	dab = devAttrGetBean(devGetBean(dev), attr);
	if (dab == NULL || dab->gpio < 0) {
		return -EFAULT;
	}
	if (dab->gpioMode == GPIO_MODE_IN) {
		return sprintf(buf, "in\n");
	}
	if (dab->gpioMode == GPIO_MODE_OUT) {
		return sprintf(buf, "out\n");
	}
	return sprintf(buf, "x\n");
}

static ssize_t devAttrTtlMode_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count) {
	size_t ret;
	int ai;
	bool* enabled;
	struct DeviceBean* db;
	struct DeviceAttrBean* dab;
	db = devGetBean(dev);
	dab = devAttrGetBean(db, attr);
	if (dab == NULL) {
		return -EFAULT;
	}

	if (w1.enabled) {
		return -EBUSY;
	}

	if (dab->gpio == GPIO_TTL1) {
		enabled = &ttl1enabled;
	} else if (dab->gpio == GPIO_TTL2) {
		enabled = &ttl2enabled;
	} else {
		return -EFAULT;
	}

	if (toUpper(buf[0]) == 'I') {
		dab->gpioMode = GPIO_MODE_IN;
	} else if (toUpper(buf[0]) == 'O') {
		dab->gpioMode = GPIO_MODE_OUT;
	} else {
		dab->gpioMode = 0;
	}

	ret = count;
	gpio_free(dab->gpio);
	if (dab->gpioMode != 0) {
		(*enabled) = true;
		if (gpioSetup(db, dab)) {
			dab->gpioMode = 0;
			gpio_free(dab->gpio);
			ret = -EFAULT;
		}
	} else {
		(*enabled) = false;
	}

	ai = 0;
	while (db->devAttrBeans[ai].devAttr.attr.name != NULL) {
		if (db->devAttrBeans[ai].gpio == dab->gpio) {
			db->devAttrBeans[ai].gpioMode = dab->gpioMode;
		}
		ai++;
	}

	return ret;
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

struct i2c_client *sensirion_i2c_client_get(uint8_t address) {
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
int8_t sensirion_i2c_write(uint8_t address, const uint8_t* data,
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
int8_t sensirion_i2c_read(uint8_t address, uint8_t* data, uint16_t count) {
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
	*temp = ((int16_t) *temp) * 100 / 256;

	return 0;
}

static int16_t thReadCalibrate(int32_t* t, int32_t* rh, int32_t* dt,
		int32_t* tCal, int32_t* rhCal) {
	int rhIdx;
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

	*dt = t16 - t9;
	if (*dt <= 0) {
		return -EIO;
	}

	// t [°C/1000]
	// rh [%/1000]
	// t9,t16,dt [°C/100]
	// temp_offset [°C/100]
	// temp_calib_b [°C/100]
	// temp_calib_m [1/1000]

	*tCal = (
			(100 * (*t)) // 100 * t [°C/1000] = t [°C/100000]
			+ (temp_calib_m * (*dt)) // tmpCalibM [1/1000] * dt [°C/100] = tmpCalibM * 1000 * dt [°C/100] = tmpCalibM * dt [°C/100000]
					+ (100 * temp_calib_b) // 100 * tmpCalibB [°C/1000] = tmpCalibB [°C/100000]
			);// [°C/100000]
	*tCal = DIV_ROUND_CLOSEST(*tCal, 1000) + temp_offset; // [°C/100]

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

static ssize_t devAttrThaTh_show(struct device* dev,
		struct device_attribute* attr, char *buf) {
	int8_t ret;
	int32_t t, rh, dt, tCal, rhCal;

	if (!exosensepi_i2c_lock()) {
		return -EBUSY;
	}

	ret = thReadCalibrate(&t, &rh, &dt, &tCal, &rhCal);

	exosensepi_i2c_unlock();

	if (ret < 0) {
		return ret;
	}

	return sprintf(buf, "%d %d %d %d %d\n", dt, t, tCal, rh, rhCal);
}

static ssize_t devAttrThaThv_show(struct device* dev,
		struct device_attribute* attr, char *buf) {
	int16_t ret;
	int32_t t, rh, dt, tCal, rhCal, voc_index;
	uint16_t sraw;

	if (!exosensepi_i2c_lock()) {
		return -EBUSY;
	}

	ret = thReadCalibrate(&t, &rh, &dt, &tCal, &rhCal);

	if (ret == NO_ERROR) {
		ret = sgp40_measure_raw_with_rht_blocking_read(rh, t, &sraw);
	}

	exosensepi_i2c_unlock();

	if (ret < 0) {
		return ret;
	}

	VocAlgorithm_process(&voc_algorithm_params, sraw, &voc_index);

	return sprintf(buf, "%d %d %d %d %d %d %d\n", dt, t, tCal, rh, rhCal, sraw,
			voc_index);
}

static ssize_t devAttrThaTempOffset_show(struct device* dev,
		struct device_attribute* attr, char *buf) {
	return sprintf(buf, "%d\n", temp_offset);
}

static ssize_t devAttrThaTempOffset_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count) {
	int ret;
	long val;

	ret = kstrtol(buf, 10, &val);
	if (ret < 0) {
		return ret;
	}

	temp_offset = val;

	return count;
}

static ssize_t devAttrLm75aU9_show(struct device* dev,
		struct device_attribute* attr,
		char *buf) {
	return devAttrLm75a_show(lm75aU9_i2c_client, dev, attr, buf);
}

static ssize_t devAttrLm75aU16_show(struct device* dev,
		struct device_attribute* attr,
		char *buf) {
	return devAttrLm75a_show(lm75aU16_i2c_client, dev, attr, buf);
}

static ssize_t devAttrLm75a_show(struct i2c_client *client, struct device* dev,
		struct device_attribute* attr, char *buf) {
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

static ssize_t opt3001_show(struct device* dev,
		struct device_attribute* attr,
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

static ssize_t devAttrWiegandEnabled_show(struct device* dev,
		struct device_attribute* attr, char *buf) {
	return sprintf(buf, w1.enabled ? "1\n" : "0\n");
}

static void wiegandReset(struct WiegandBean* w) {
	w->enabled = true;
	w->data = 0;
	w->bitCount = 0;
	w->activeLine = NULL;
	w->d0.wasLow = false;
	w->d1.wasLow = false;
}

static unsigned long to_usec(struct timespec64 *t) {
	return (t->tv_sec * 1000000) + (t->tv_nsec / 1000);
}

static unsigned long diff_usec(struct timespec64 *t1, struct timespec64 *t2) {
	struct timespec64 diff;
	diff = timespec64_sub(*t2, *t1);
	return to_usec(&diff);
}

static irq_handler_t wiegandDataIrqHandler(unsigned int irq, void *dev_id,
		struct pt_regs *regs) {
	bool isLow;
	struct timespec64 now;
	unsigned long diff;
	struct WiegandLine* l = NULL;

	if (w1.enabled) {
		if (irq == w1.d0.irq) {
			l = &w1.d0;
		} else if (irq == w1.d1.irq) {
			l = &w1.d1;
		}
	}

	if (l == NULL) {
		return (irq_handler_t) IRQ_HANDLED;
	}

	isLow = gpio_get_value(l->gpio) == 0;

	ktime_get_raw_ts64(&now);

	if (l->wasLow == isLow) {
		// got the interrupt but didn't change state. Maybe a fast pulse
		w1.noise = 10;
		return (irq_handler_t) IRQ_HANDLED;
	}

	l->wasLow = isLow;

	if (isLow) {
		if (w1.bitCount != 0) {
			diff = diff_usec((struct timespec64 *) &w1.lastBitTs, &now);

			if (diff < w1.pulseIntervalMin_usec) {
				// pulse too early
				w1.noise = 11;
				goto noise;
			}

			if (diff > w1.pulseIntervalMax_usec) {
				w1.data = 0;
				w1.bitCount = 0;
			}
		}

		if (w1.activeLine != NULL) {
			// there's movement on both lines
			w1.noise = 12;
			goto noise;
		}

		w1.activeLine = l;

		w1.lastBitTs.tv_sec = now.tv_sec;
		w1.lastBitTs.tv_nsec = now.tv_nsec;

	} else {
		if (w1.activeLine != l) {
			// there's movement on both lines or previous noise
			w1.noise = 13;
			goto noise;
		}

		w1.activeLine = NULL;

		if (w1.bitCount >= WIEGAND_MAX_BITS) {
			return (irq_handler_t) IRQ_HANDLED;
		}

		diff = diff_usec((struct timespec64 *) &w1.lastBitTs, &now);
		if (diff < w1.pulseWidthMin_usec) {
			// pulse too short
			w1.noise = 14;
			goto noise;
		}
		if (diff > w1.pulseWidthMax_usec) {
			// pulse too long
			w1.noise = 15;
			goto noise;
		}

		w1.data <<= 1;
		if (l == &w1.d1) {
			w1.data |= 1;
		}
		w1.bitCount++;
	}

	return (irq_handler_t) IRQ_HANDLED;

	noise:
	wiegandReset(&w1);
	return (irq_handler_t) IRQ_HANDLED;
}

static void wiegandDisable(struct WiegandBean* w) {
	if (w->enabled) {
		gpio_free(w->d0.gpio);
		gpio_free(w->d1.gpio);

		if (w->d0.irqRequested) {
			free_irq(w->d0.irq, NULL);
			w->d0.irqRequested = false;
		}

		if (w->d1.irqRequested) {
			free_irq(w->d1.irq, NULL);
			w->d1.irqRequested = false;
		}

		w->enabled = false;
	}
}

static ssize_t devAttrWiegandEnabled_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count) {
	bool enable;
	int result = 0;
	char reqName[] = "exosensepi_ttN";

	if (buf[0] == '0') {
		enable = false;
	} else if (buf[0] == '1') {
		enable = true;
	} else {
		return -EINVAL;
	}

	if (enable) {
		if (ttl1enabled || ttl2enabled) {
			return -EBUSY;
		}

		reqName[13] = '0';
		gpio_request(w1.d0.gpio, reqName);
		reqName[13] = '1';
		gpio_request(w1.d1.gpio, reqName);

		result = gpio_direction_input(w1.d0.gpio);
		if (!result) {
			result = gpio_direction_input(w1.d1.gpio);
		}

		if (result) {
			printk(
			KERN_ALERT "exosensepi: * | error setting up wiegand GPIOs\n");
			enable = false;
		} else {
			gpio_set_debounce(w1.d0.gpio, 0);
			gpio_set_debounce(w1.d1.gpio, 0);

			w1.d0.irq = gpio_to_irq(w1.d0.gpio);
			w1.d1.irq = gpio_to_irq(w1.d1.gpio);

			reqName[13] = '0';
			result = request_irq(w1.d0.irq,
					(irq_handler_t) wiegandDataIrqHandler,
					IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
					reqName, NULL);

			if (result) {
				printk(
						KERN_ALERT "exosensepi: * | error registering wiegand D0 irq handler\n");
				enable = false;
			} else {
				w1.d0.irqRequested = true;

				reqName[13] = '1';
				result = request_irq(w1.d1.irq,
						(irq_handler_t) wiegandDataIrqHandler,
						IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
						reqName, NULL);

				if (result) {
					printk(
							KERN_ALERT "exosensepi: * | error registering wiegand D1 irq handler\n");
					enable = false;
				} else {
					w1.d1.irqRequested = true;
				}
			}
		}
	}

	if (enable) {
		w1.noise = 0;
		wiegandReset(&w1);
	} else {
		wiegandDisable(&w1);
	}

	if (result) {
		return result;
	}
	return count;
}

static ssize_t devAttrWiegandData_show(struct device* dev,
		struct device_attribute* attr, char *buf) {
	struct timespec64 now;
	unsigned long diff;

	if (!w1.enabled) {
		return -ENODEV;
	}

	ktime_get_raw_ts64(&now);
	diff = diff_usec((struct timespec64 *) &w1.lastBitTs, &now);
	if (diff <= w1.pulseIntervalMax_usec) {
		return -EBUSY;
	}

	return sprintf(buf, "%lu %d %llu\n", to_usec(&w1.lastBitTs), w1.bitCount,
			w1.data);
}

static ssize_t devAttrWiegandNoise_show(struct device* dev,
		struct device_attribute* attr, char *buf) {
	int noise;
	noise = w1.noise;
	w1.noise = 0;
	return sprintf(buf, "%d\n", noise);
}

static ssize_t devAttrWiegandPulseIntervalMin_show(struct device* dev,
		struct device_attribute* attr, char *buf) {
	return sprintf(buf, "%lu\n", w1.pulseIntervalMin_usec);
}

static ssize_t devAttrWiegandPulseIntervalMin_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count) {
	int ret;
	unsigned long val;

	ret = kstrtol(buf, 10, &val);
	if (ret < 0) {
		return ret;
	}

	w1.pulseIntervalMin_usec = val;

	return count;
}

static ssize_t devAttrWiegandPulseIntervalMax_show(struct device* dev,
		struct device_attribute* attr, char *buf) {
	return sprintf(buf, "%lu\n", w1.pulseIntervalMax_usec);
}

static ssize_t devAttrWiegandPulseIntervalMax_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count) {
	int ret;
	unsigned long val;

	ret = kstrtol(buf, 10, &val);
	if (ret < 0) {
		return ret;
	}

	w1.pulseIntervalMax_usec = val;

	return count;
}

static ssize_t devAttrWiegandPulseWidthMin_show(struct device* dev,
		struct device_attribute* attr, char *buf) {
	return sprintf(buf, "%lu\n", w1.pulseWidthMin_usec);
}

static ssize_t devAttrWiegandPulseWidthMin_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count) {
	int ret;
	unsigned long val;

	ret = kstrtol(buf, 10, &val);
	if (ret < 0) {
		return ret;
	}

	w1.pulseWidthMin_usec = val;

	return count;
}

static ssize_t devAttrWiegandPulseWidthMax_show(struct device* dev,
		struct device_attribute* attr, char *buf) {
	return sprintf(buf, "%lu\n", w1.pulseWidthMax_usec);
}

static ssize_t devAttrWiegandPulseWidthMax_store(struct device* dev,
		struct device_attribute* attr, const char *buf, size_t count) {
	int ret;
	unsigned long val;

	ret = kstrtol(buf, 10, &val);
	if (ret < 0) {
		return ret;
	}

	w1.pulseWidthMax_usec = val;

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
MODULE_DEVICE_TABLE(of, exosensepi_of_match);

static const struct i2c_device_id exosensepi_i2c_id[] = {
	{ "exosensepi", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, exosensepi_i2c_id);

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

static irqreturn_t gpio_deb_irq_handler(int irq, void *dev_id) {
	struct timespec64 now;
	int db = 0;
	unsigned long diff;
	int actualGPIOStatus;

	ktime_get_raw_ts64(&now);

	while (debounceBeans[db].debIrqDevName != NULL) {
		if (debounceBeans[db].debIrqNum == irq && debounceBeans[db].gpio != 0) {
			actualGPIOStatus = gpio_get_value(debounceBeans[db].gpio);

			diff = diff_usec(
					(struct timespec64*) &debounceBeans[db].lastDebIrqTs, &now);

			if (debounceBeans[db].debPastValue == actualGPIOStatus) {
				return IRQ_HANDLED;
			}

			debounceBeans[db].debPastValue = actualGPIOStatus;

			if (actualGPIOStatus) {
				if (diff >= debounceBeans[db].debOffMinTime_usec) {
					debounceBeans[db].debValue = 0;
					debounceBeans[db].debOffStateCnt =
							debounceBeans[db].debOffStateCnt >= ULONG_MAX ?
									0 : debounceBeans[db].debOffStateCnt + 1;
				}
			} else {
				if (diff >= debounceBeans[db].debOnMinTime_usec) {
					debounceBeans[db].debValue = 1;
					debounceBeans[db].debOnStateCnt =
							debounceBeans[db].debOnStateCnt >= ULONG_MAX ?
									0 : debounceBeans[db].debOnStateCnt + 1;
				}
			}
			ktime_get_raw_ts64(&debounceBeans[db].lastDebIrqTs);
			break;
		}
		db++;
	}

	return IRQ_HANDLED;
}

static void cleanup(void) {
	int di, ai;

	i2c_del_driver(&exosensepi_i2c_driver);
	mutex_destroy(&exosensepi_i2c_mutex);

	di = 0;
	while (devices[di].name != NULL) {
		if (devices[di].pDevice && !IS_ERR(devices[di].pDevice)) {
			ai = 0;
			while (devices[di].devAttrBeans[ai].devAttr.attr.name != NULL) {
				device_remove_file(devices[di].pDevice,
						&devices[di].devAttrBeans[ai].devAttr);
				if (devices[di].devAttrBeans[ai].gpioMode != 0) {
					gpio_free(devices[di].devAttrBeans[ai].gpio);
				}
				if (devices[di].devAttrBeans[ai].debBean != NULL){
					free_irq(devices[di].devAttrBeans[ai].debBean->debIrqNum, NULL);
				}
				ai++;
			}
		}
		device_destroy(pDeviceClass, 0);
		di++;
	}

	if (!IS_ERR(pDeviceClass)) {
		class_destroy(pDeviceClass);
	}

	wiegandDisable(&w1);
}

static int __init exosensepi_init(void) {
	int result = 0;
	int di, ai;

	printk(KERN_INFO "exosensepi: - | init\n");

	i2c_add_driver(&exosensepi_i2c_driver);
	mutex_init(&exosensepi_i2c_mutex);

	VocAlgorithm_init(&voc_algorithm_params);

	pDeviceClass = class_create(THIS_MODULE, "exosensepi");
	if (IS_ERR(pDeviceClass)) {
		printk(KERN_ALERT "exosensepi: * | failed to create device class\n");
		goto fail;
	}

	di = 0;
	while (devices[di].name != NULL) {
		devices[di].pDevice = device_create(pDeviceClass, NULL, 0, NULL,
				devices[di].name);
		if (IS_ERR(devices[di].pDevice)) {
			printk(KERN_ALERT "exosensepi: * | failed to create device '%s'\n",
					devices[di].name);
			goto fail;
		}

		ai = 0;
		while (devices[di].devAttrBeans[ai].devAttr.attr.name != NULL) {
			result = device_create_file(devices[di].pDevice,
					&devices[di].devAttrBeans[ai].devAttr);
			if (result) {
				printk(
						KERN_ALERT "exosensepi: * | failed to create device file '%s/%s'\n",
						devices[di].name,
						devices[di].devAttrBeans[ai].devAttr.attr.name);
				goto fail;
			}
			if (devices[di].devAttrBeans[ai].gpioMode != 0) {
				result = gpioSetup(&devices[di], &devices[di].devAttrBeans[ai]);
				if (result) {
					printk(
					KERN_ALERT "exosensepi: * | error setting up GPIO %d\n",
							devices[di].devAttrBeans[ai].gpio);
					goto fail;
				}
			}
			if (devices[di].devAttrBeans[ai].debBean != NULL){
				if (!devices[di].devAttrBeans[ai].debBean->debIrqNum){
					devices[di].devAttrBeans[ai].debBean->debIrqNum = gpio_to_irq(devices[di].devAttrBeans[ai].debBean->gpio);
					if (request_irq(devices[di].devAttrBeans[ai].debBean->debIrqNum,
						(void *) gpio_deb_irq_handler,
								IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
								devices[di].devAttrBeans[ai].debBean->debIrqDevName,
								NULL)) {
						printk("exosensepi: * | cannot register IRQ of %s in device %s\n", devices[di].devAttrBeans[ai].devAttr.attr.name, devices[di].name);
						goto fail;
					}
					ktime_get_raw_ts64(&devices[di].devAttrBeans[ai].debBean->lastDebIrqTs);
					devices[di].devAttrBeans[ai].debBean->debValue = DEBOUNCE_STATE_NOT_DEFINED;
					devices[di].devAttrBeans[ai].debBean->debPastValue = gpio_get_value(devices[di].devAttrBeans[ai].debBean->gpio);
				}
			}
			ai++;
		}
		di++;
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

module_init(exosensepi_init);
module_exit(exosensepi_exit);
