include ../../Makefile.in

TARGET = multi_pipe_vin_isp_vse

SRCS := $(foreach cf, $(SRC_PATH), $(wildcard $(cf)/*.c))
OBJS :=  $(SRCS:.c=.o)

.PHONY: all clean


%.o:%.c
	@mkdir -p $(abspath $(dir $@))
	$(CC) $(INCS) $(CFLAGS) -c $< $(LIBS) $(LIBSDIR) -o $@


$(TARGET):$(OBJS)
	@mkdir -p $(abspath $(dir $@))
	$(CC) -O0 -o $@ $(OBJS) $(CFLAGS) -lmultimedia ${LIBSDIR} $(LIBS)

all: ${TARGET}

install: ${TARGET}
	@echo "Generate deployment package"
	@mkdir -p install
	@cp -arf ${TARGET} install/
	@cp -arf config install/
	@cp -arf generate_custom_config/ install/

clean:
	rm -rf ${OBJS} ${TARGET} install
