include ${SRC_BUILD_DIR}/core/Makefile

ifeq (${BUILD_OUTPUT_PATH},)
export BUILD_OUTPUT_PATH=$(shell pwd)
endif

.PHONY: all all_32 clean

SRC = $(wildcard *.c)

OBJECT := ${BUILD_OUTPUT_PATH}/staging/ffdemux/ffdemux
OBJS := $(addprefix ${BUILD_OUTPUT_PATH}/staging/ffdemux/,$(SRC:.c=.o))

INCS += -I./inc
LIBS += -lpthread -lavformat -lavcodec -lavutil

all all_32: $(OBJECT)

$(OBJECT): $(OBJS)
	@mkdir -p $(abspath $(dir $@))
	$(CC) $(CFLAGS) $^ $(LDFLAGS) $(LIBS) -o $@

${BUILD_OUTPUT_PATH}/staging/ffdemux/%.o: %.c
	@mkdir -p $(abspath $(dir $@))
	$(CC) $(CFLAGS) -c $< $(INCS) -o $@

clean:
	rm -rf $(OBJECT) $(OBJS)
