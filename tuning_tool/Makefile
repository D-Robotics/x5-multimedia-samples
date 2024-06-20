include ../Makefile.in
CUR_DIR := $(shell pwd)
RELATIVE_PATH := $(shell realpath --relative-to=$(PLATFORM_SAMPLES_DIR) $(CUR_DIR))
TUNING_BUILD_DIR := out
CFG_FILE := cfg
SH_FILE := run_tuning.sh

TARGET := ${TUNING_BUILD_DIR}/isp_tuning
SRCS := $(foreach cf, ./, $(wildcard $(cf)/*.c))
OBJS := $(patsubst %.c,${TUNING_BUILD_DIR}/%.o,$(SRCS))

LIBS += -levent_pthreads -lhbplayer -lpthread -levent

.PHONY: all clean


${TUNING_BUILD_DIR}/%.o:%.c
	@mkdir -p $(abspath $(dir $@))
	@$(CC) $(INCS) $(CFLAGS) -c $< $(LIBS) $(LIBSDIR) -o $@

$(TARGET):$(OBJS)
	@$(shell mkdir -p ${TUNING_BUILD_DIR})
	@$(CC) -O0 -o $@ $(OBJS) $(CFLAGS) ${LIBSDIR} $(LIBS)

all: ${TARGET}

clean:
	rm -rf ${TUNING_BUILD_DIR}

install: ${TARGET}
	$(Q)install -d ${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}
	$(Q)install -m 0775 ${TARGET} ${SH_FILE} \
		${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/
	$(Q)install -d ${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/tuning_cfg/
	$(Q)cp -r ${CFG_FILE}/* ${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/tuning_cfg/
