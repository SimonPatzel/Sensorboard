CC      := gcc
TARGET  := main
RAYLIB  := lib/raylib/src
SRC     := main.c
HDRS    := $(wildcard include/*.h)

CFLAGS  := -std=c99 -Wall -Og -I. -Iinclude -I$(RAYLIB)
LDFLAGS := -L$(RAYLIB)
LDLIBS  := -lraylib -lGL -lm -lpthread -ldl -lrt -lX11 -lcrypto

$(TARGET): $(SRC) $(HDRS) $(RAYLIB)/libraylib.a
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS) $(LDLIBS)

$(RAYLIB)/libraylib.a:
	$(MAKE) -C $(RAYLIB) PLATFORM=PLATFORM_DESKTOP

clean:
	rm -f $(TARGET)
	$(MAKE) -C $(RAYLIB) clean

.PHONY: clean
