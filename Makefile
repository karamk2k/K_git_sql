CC = gcc
CFLAGS = -Wall -Wextra -Iinclude $(shell pkg-config --cflags mysqlclient libgit2)
LDFLAGS = $(shell pkg-config --libs mysqlclient libgit2) -lpthread

BUILD_DIR = build
TARGET = $(BUILD_DIR)/main
SRC = src/main.c src/mysql_service.c

all: $(TARGET)

$(TARGET): $(SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

build: all

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean build run
