CC      := gcc
TARGET  := main
SRC     := main.c
HDRS    := $(wildcard include/*.h)

# --- Plattform-Auswahl -------------------------------------------------
# Nutzung:  make PLATFORM=pi   oder   make PLATFORM=laptop
# Ohne Angabe wird nachgefragt.
ifeq ($(PLATFORM),)
  $(info Plattform wählen: [1] Linux-Laptop  [2] Raspberry Pi)
  CHOICE := $(shell read -p "Auswahl (1/2): " c; echo $$c)
  ifeq ($(CHOICE),2)
    PLATFORM := pi
  else
    PLATFORM := laptop
  endif
endif

ifeq ($(PLATFORM),pi)
  # Raspberry Pi mit Desktop: GLFW/X11, aber OpenGL ES 2
  RAYLIB       := lib/raylib-pi/src
  RAYLIB_FLAGS := PLATFORM=PLATFORM_DESKTOP GRAPHICS=GRAPHICS_API_OPENGL_ES2
  PLAT_LIBS    := -lGLESv2 -lEGL -lX11
else
  # Linux-Laptop: GLFW/X11, normales OpenGL
  RAYLIB       := lib/raylib/src
  RAYLIB_FLAGS := PLATFORM=PLATFORM_DESKTOP
  PLAT_LIBS    := -lGL -lX11
endif

CFLAGS  := -std=c99 -Wall -Og -I. -Iinclude -I$(RAYLIB)
LDFLAGS := -L$(RAYLIB)
LDLIBS  := -lraylib $(PLAT_LIBS) -lm -lpthread -ldl -lrt -lcrypto

$(TARGET): $(SRC) $(HDRS) $(RAYLIB)/libraylib.a
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS) $(LDLIBS)

$(RAYLIB)/libraylib.a:
	$(MAKE) -C $(RAYLIB) $(RAYLIB_FLAGS)

clean:
	rm -f $(TARGET)
	-$(MAKE) -C lib/raylib/src clean
	-$(MAKE) -C lib/raylib-pi/src clean

.PHONY: clean
