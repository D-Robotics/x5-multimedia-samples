# include ../../../Makefile.in
ifeq ($(BOARD), x86)
$(warning "Build Sample In x86")
else
include ../../../Makefile.in
endif

CC = ${CROSS_COMPILE}gcc
CXX = ${CROSS_COMPILE}g++
AR = ${CROSS_COMPILE}ar
AS = ${CROSS_COMPILE}as
LD = ${CROSS_COMPILE}ld

STRIP = ${CROSS_COMPILE}strip
RANLIB = ${CROSS_COMPILE}ranlib
OBJCOPY = ${CROSS_COMPILE}objcopy
OBJDUMP = ${CROSS_COMPILE}objdump
READELF = ${CROSS_COMPILE}readelf
NM = ${CROSS_COMPILE}nm

CFLAGS = -Wall -Werror
CXXFLAGS = -Wall -Werror
CFLAGS_DYNAMIC = -fPIC -shared
CFLAGS_STATIC = -rcs
LDFLAGS =

PLATFORM_SAMPLES_DIR := $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

HR_TOP_DIR = $(shell realpath ${PLATFORM_SAMPLES_DIR}/../../../)

HR_BUILD_OUTPUT_DIR = ${HR_TOP_DIR}/out
$(info HR_BUILD_OUTPUT_DIR not set so use default path: [$(HR_BUILD_OUTPUT_DIR)])

CUR_DIR := $(shell pwd)
RELATIVE_PATH := $(shell realpath --relative-to=$(PLATFORM_SAMPLES_DIR) $(CUR_DIR))

TARGET = multi_pipe_depth2cloud
RUN_SHELL = run_client.sh

SRC_PATH = $(CUR_DIR)

SRCS := $(foreach cf, $(SRC_PATH), $(wildcard $(cf)/*.c $(cf)/*.cpp))
SRCS += ${CUR_DIR}/../common/cJSON.c \
		${CUR_DIR}/../common/pipe_stream.c
SRCS += $(CUR_DIR)/../../common/performance_test_util.c \
		$(CUR_DIR)/../../common/synchronous_queue.c \
		$(CUR_DIR)/../../common/mqueue.c

OBJS := $(patsubst %.cpp,%.obj,$(patsubst %.c,%.o,$(SRCS)))
DEP := $(patsubst %.obj,%.dep,$(patsubst %.o,%.d,$(OBJS)))

INCS += -I$(CUR_DIR)/../../common \
		-I$(CUR_DIR)/../common


# LIBS += -ldnn -lcnn_intf -lhbrt_bayes_aarch64 -lstdc++ -lopencv_core -lopencv_imgcodecs -lopencv_imgproc
# LIBSDIR += -L${CUR_DIR}/../3rdparty/eigen3 \
# 		-L${CUR_DIR}/../3rdparty/libdnn/hobot/lib \
# 		-L${CUR_DIR}/../3rdparty/lib_opencv4.5.4/lib
# CXXFLAGS += -Wno-class-memaccess -Wno-sign-compare -O3 -DNDEBUG

# $(warning LIBS=$(LIBS))
# $(warning LIBSDIR=$(LIBSDIR))
# $(warning CFLAGS=$(CFLAGS))
# $(warning INCS=$(INCS))
# $(warning SRCS=$(SRCS))
# $(warning OBJS=$(OBJS))

# libpng
# LIBS += -lpng -lz
# INCS += -I/home/share/project/x5/out/deploy/system/usr/include

.PHONY: all clean install

%.o:%.c
	@mkdir -p $(abspath $(dir $@))
	$(CC) $(INCS) $(CFLAGS) -c $< $(LIBS) $(LIBSDIR) -o $@

%.obj:%.cpp
	$(CXX) $(CXXFLAGS) $(INCS) $(LIBS) $(LIBSDIR) -MMD -MP -MF"$(@:%.obj=%.dep)" -MT"$(@:%.obj=%.dep)" -c $< -o $@

$(TARGET):$(OBJS)
	@mkdir -p $(abspath $(dir $@))
	$(CC) -O0 -o $@ $(OBJS) $(CFLAGS) -lpthread ${LIBSDIR} $(LIBS)

all: ${TARGET}

clean:
	rm -rf ${OBJS} ${DEP} ${TARGET} install

install: ${TARGET}
	$(Q)install -d ${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}
	$(Q)install -m 0775 ${TARGET} \
		${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/
	$(Q)install -m 0775 ${RUN_SHELL} \
		${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/
