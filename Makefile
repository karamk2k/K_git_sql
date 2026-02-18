CC = gcc
CFLAGS = -Wall -Wextra -Iinclude $(shell pkg-config --cflags mysqlclient libgit2)
LDFLAGS = $(shell pkg-config --libs mysqlclient libgit2) -lpthread

BUILD_DIR = build
TARGET = $(BUILD_DIR)/main
SRC = src/main.c src/mysql_service.c src/git_service.c src/app_context.c

all: $(TARGET)

$(TARGET): $(SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

build: all

run: $(TARGET)
	./$(TARGET)

run-d: $(TARGET)
	@mkdir -p $(BUILD_DIR)
	@nohup ./$(TARGET) > $(BUILD_DIR)/main.log 2>&1 & echo $$! > $(BUILD_DIR)/main.pid
	@echo "Started $(TARGET) in background (PID $$(cat $(BUILD_DIR)/main.pid))"
	@echo "Logs: $(BUILD_DIR)/main.log"

stop-d:
	@if [ -f $(BUILD_DIR)/main.pid ]; then \
		kill $$(cat $(BUILD_DIR)/main.pid) && rm -f $(BUILD_DIR)/main.pid && echo "Stopped background process"; \
	else \
		echo "No PID file found"; \
	fi

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean build run run-d stop-d
