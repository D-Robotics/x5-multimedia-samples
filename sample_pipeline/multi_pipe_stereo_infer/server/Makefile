include ../../../Makefile.in
CUR_DIR := $(shell pwd)
RELATIVE_PATH := $(shell realpath --relative-to=$(PLATFORM_SAMPLES_DIR) $(CUR_DIR))

TARGET = multi_pipe_stereo_infer
RUN_SHELL = run_server.sh
MODEL_BIN = DStereoV2.6_int8.bin
HOBOT_STEREONET_DIR = hobot_stereonet

SRC_PATH += ${CUR_DIR}/../common
SRCS := $(foreach cf, $(SRC_PATH), $(wildcard $(cf)/*.c $(cf)/*.cpp))

SRCS += $(CUR_DIR)/../../common/performance_test_util.c \
		$(CUR_DIR)/../../common/synchronous_queue.c \
		$(CUR_DIR)/../../common/mqueue.c \
		$(CUR_DIR)/$(HOBOT_STEREONET_DIR)/src/stereonet_process.cpp \
		$(CUR_DIR)/$(HOBOT_STEREONET_DIR)/src/timer_utils.cpp \
		$(CUR_DIR)/$(HOBOT_STEREONET_DIR)/src/img_convert_utils.cpp


OBJS := $(patsubst %.cpp,%.obj,$(patsubst %.c,%.o,$(SRCS)))
DEP := $(patsubst %.obj,%.dep,$(patsubst %.o,%.d,$(OBJS)))

# Stereo Infer
LIB_3RDPART_DIR := $(CUR_DIR)/$(HOBOT_STEREONET_DIR)/DStereo_X5/3rdparty
INCS += -I$(CUR_DIR)/../common \
		-I$(CUR_DIR)/../../common \
		-I${HR_BUILD_OUTPUT_DIR}/deploy/system/usr/include/ \
		-I$(LIB_3RDPART_DIR)/eigen3 \
		-I$(LIB_3RDPART_DIR)/lib_opencv4.5.4/include/opencv4 \
		-I$(CUR_DIR)/$(HOBOT_STEREONET_DIR)/include \
		-I$(CUR_DIR)/$(HOBOT_STEREONET_DIR)/DStereo_X5/build/_deps/magic_enum-src/include


LIBS += -ldnn -lcnn_intf -lhbrt_bayes_aarch64 -lstdc++ -lopencv_core \
		-lopencv_imgcodecs -lopencv_imgproc -lopencv_features2d -lopencv_flann -lopencv_calib3d
LIBSDIR += -L${LIB_3RDPART_DIR}/eigen3 \
		-L${HR_BUILD_OUTPUT_DIR}/deploy/system/usr/lib \
		-L${LIB_3RDPART_DIR}/lib_opencv4.5.4/lib

# libdrm
LIBS += -lm -ldrm
INCS += -I ${HR_BUILD_OUTPUT_DIR}/deploy/system/usr/include/libdrm/
INCS += -I ${HR_BUILD_OUTPUT_DIR}/deploy/system/usr/include/

CXXFLAGS += -Wno-class-memaccess -Wno-sign-compare -O3 -DNDEBUG -Wno-unused-function -DPLATFORM_X5

# $(warning LIBS=$(LIBS))
# $(warning LIBSDIR=$(LIBSDIR))
# $(warning CFLAGS=$(CFLAGS))
# $(warning INCS=$(INCS))
# $(warning SRCS=$(SRCS))
# $(warning OBJS=$(OBJS))

all: check_hobot_stereonet ${TARGET}

check_hobot_stereonet:
	@echo "Check ${HOBOT_STEREONET_DIR}..."
	@if [ ! -d "${HOBOT_STEREONET_DIR}" ]; then \
		echo "Error: ${HOBOT_STEREONET_DIR} directory not found."; \
		echo "Please run: "; \
		echo "    1. git clone https://github.com/D-Robotics/hobot_stereonet.git"; \
		echo "    2. git checkout api_0.0.1"; \
		echo "    3. cd hobot_stereonet/DStereo_X5; bash ./run_build.sh" \
		echo "    4. cd - ; bash ./make_ln.sh"; \
		exit 1; \
	fi

.PHONY: all check_hobot_stereonet clean

%.o:%.c
	@mkdir -p $(abspath $(dir $@))
	$(CC) $(INCS) $(CFLAGS) -c $< $(LIBS) $(LIBSDIR) -o $@

%.obj:%.cpp
	$(CXX) $(CXXFLAGS) $(INCS) $(LIBS) $(LIBSDIR) -MMD -MP -MF"$(@:%.obj=%.dep)" -MT"$(@:%.obj=%.dep)" -c $< -o $@

$(TARGET):$(OBJS)
	@echo "Building project..."
	@mkdir -p $(abspath $(dir $@))
	$(CC) -O0 -o $@ $(OBJS) $(CFLAGS) -lmultimedia ${LIBSDIR} $(LIBS)

clean:
	rm -rf ${OBJS} ${DEP} ${TARGET}

# install: ${TARGET}
# 	$(Q)install -d ${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}
# 	$(Q)install -m 0775 ${TARGET} \
# 		${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/
# 	# $(Q)install -m 0775 ${RUN_SHELL} \
# 	# 	${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/
# 	# $(Q)install ${MODEL_BIN} \
# 	# 	${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/
# 	# $(Q)cp -rf ${RUNLIBS_DIR} \
# 	# 	${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/../
