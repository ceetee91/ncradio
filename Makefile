-include config.mk

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -D_POSIX_C_SOURCE=199309L $(AUDIO_CFLAGS)
LDFLAGS = -lncurses -lpthread $(AUDIO_LIBS)

TARGET  = ncradio
SRCS    = ncradio.c radio.c config.c rds.c $(AUDIO_SRCS)
OBJS    = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

ncradio.o: ncradio.c radio.h config.h audio.h
radio.o:   radio.c   radio.h rds.h config.h
config.o:  config.c  config.h
rds.o:     rds.c     rds.h
audio.o:   audio.c   audio.h

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

clean:
	rm -f $(OBJS) $(TARGET)

distclean: clean
	rm -f config.mk

.PHONY: all install clean distclean
