include ../Makefile.in

TARGET = sample_codec

SRCS := $(foreach cf, $(SRC_PATH), $(wildcard $(cf)/*.c))
OBJS :=  $(SRCS:.c=.o)

INCS += -I ${HR_BUILD_OUTPUT_DIR}/deploy/system/usr/include/
LIBS += -lmultimedia -lavformat -lavcodec -lavutil -lssl -lcrypto -ldrm -lz

.PHONY: all clean

%.o:%.c
	@mkdir -p $(abspath $(dir $@))
	$(CC) -o $@ $(INCS) $(CFLAGS) -c $<

$(TARGET):$(OBJS)
	@mkdir -p $(abspath $(dir $@))
	$(CC) -O0 -o $@ $(OBJS) $(CFLAGS) ${LIBSDIR} $(LIBS)

all: ${TARGET}

clean:
	rm -rf ${OBJS} ${TARGET}
