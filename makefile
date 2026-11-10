CC = gcc
CFLAGS = -Wall -Wextra -std=c17 -D_POSIX_C_SOURCE=200112L
LDLIBS = -lX11

TARGET = dist/main
SRC = src/wm.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(SRC) -o $(TARGET) $(CFLAGS) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
