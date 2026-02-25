CC = gcc
CFLAGS = -Wall -Wextra -O2
TARGET = webserver
SRC = webserver.c

.PHONY: all build run clean help

all: build

build: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

run: build
	./$(TARGET)

clean:
	rm -f $(TARGET)

help:
	@echo "Available targets:"
	@echo "  make build  - Compile the webserver"
	@echo "  make run    - Build and run the webserver"
	@echo "  make clean  - Remove compiled files"
	@echo "  make help   - Show this help message"
