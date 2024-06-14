
GLOBAL_INSTALL_DIR := $(PRO_ROOT)sunrise_camera
CROSS_COMPILE ?= /opt/arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-
COMPILE_PREFIX := $(CROSS_COMPILE)
CFLAGS_EX  := -Wall -g -O2 -fstack-protector

BUILD_OUT_DIR ?= $(PRO_ROOT)../../../../out/build

HBRE_LIB ?= $(BUILD_OUT_DIR)/hbre_deps/usr/lib
HBRE_INC ?= $(BUILD_OUT_DIR)/hbre_deps

CHIP_ID ?= CHIP_X5_SOM
############################################################
# MODULE_SYSTEM := y
MODULE_VPP := y
# MODULE_NETWORK := y
# MODULE_RECORD := y
# MODULE_ALARM := y
MODULE_RTSP := y
MODULE_WEBSOCKET := y

subdir :=
subdir += common
subdir += communicate
# subdir += Transport
# subdir += Record

ifeq ($(MODULE_SYSTEM), y)
	CFLAGS_EX += -DMODULE_SYSTEM
	subdir += System
endif
ifeq ($(MODULE_VPP), y)
	CFLAGS_EX += -DMODULE_VPP
	subdir += Platform/$(PLATFORM)
endif
ifeq ($(MODULE_NETWORK), y)
	CFLAGS_EX += -DMODULE_NETWORK
	subdir += Network
endif
ifeq ($(MODULE_RECORD), y)
	CFLAGS_EX += -DMODULE_RECORD
endif
ifeq ($(MODULE_ALARM), y)
	CFLAGS_EX += -DMODULE_ALARM
	subdir += Alarm
endif
ifeq ($(MODULE_RTSP), y)
	CFLAGS_EX += -DMODULE_RTSP
	subdir += Transport/rtspserver/live555
	subdir += Transport/rtspserver
endif
ifeq ($(MODULE_WEBSOCKET), y)
	CFLAGS_EX += -DMODULE_WEBSOCKET
	subdir += Transport/websocket
endif
subdir += main

############################################################
ifeq ($(MODULE_VPP), y)
	PLATFORM_LIBS_NAME := cam vpf hbmem multimedia avformat avcodec avutil swresample ffmedia gdcbin cjson alog dnn cnn_intf hbrt_bayes_aarch64 ssl crypto drm z dl rt pthread
	PLATFORM_LIBS += $(patsubst %,-l%,$(PLATFORM_LIBS_NAME))
	LDFLAGS_EX += -L$(HBRE_LIB) -L$(PRO_ROOT)../../../../out/deploy/system/usr/lib
endif

GLOBAL_EXTERN_INC_DIR += $(HBRE_INC) $(PRO_ROOT)../../../../out/deploy/system/usr
