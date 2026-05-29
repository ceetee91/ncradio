CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -D_POSIX_C_SOURCE=199309L
LDFLAGS = -lncurses -lpthread

TARGET  = ncradio
SRCS    = ncradio.c radio.c config.c rds.c
OBJS    = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

ncradio.o: ncradio.c radio.h config.h
radio.o:   radio.c   radio.h rds.h config.h
config.o:  config.c  config.h
rds.o:     rds.c     rds.h

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install clean
