MODULE_MAIN_OBJ := module.o
COMMON_MODULES := utils gpio wiegand atecc
MODULE_EXTRA_OBJS := sensirion/common/sensirion_common.o \
                     sensirion/sht4x/sht4x.o \
                     sensirion/sht4x/sht_git_version.o \
                     sensirion/sgp40/sgp40.o \
                     sensirion/sgp40/sgp_git_version.o \
                     sensirion/sgp40_voc_index/sensirion_voc_algorithm.o
UDEV_RULES := 99-exosensepi.rules
UDEV_GROUP := exosensepi

SOURCE_DIR := $(if $(src),$(src),$(CURDIR))
include $(SOURCE_DIR)/commons/scripts/kmod-common.mk
