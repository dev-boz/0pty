CC ?= cc
SRC_DIR := src
BIN_DIR := bin

CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror
CPPFLAGS += -I$(SRC_DIR) -MMD -MP

CLIENT_SRCS := \
	$(SRC_DIR)/client.c \
	$(SRC_DIR)/common.c \
	$(SRC_DIR)/protocol.c

SERVER_SRCS := \
	$(SRC_DIR)/server.c \
	$(SRC_DIR)/common.c \
	$(SRC_DIR)/protocol.c \
	$(SRC_DIR)/ring.c

CLIENT_OBJS := $(CLIENT_SRCS:.c=.o)
SERVER_OBJS := $(SERVER_SRCS:.c=.o)
TEST_BIN := $(BIN_DIR)/protocol-ring-test

CLIENT_LDLIBS := -pthread
SERVER_LDLIBS := -pthread -lutil

all: $(BIN_DIR)/0pty $(BIN_DIR)/0pty-server

$(BIN_DIR):
	mkdir -p $@

$(BIN_DIR)/0pty: $(CLIENT_OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) $(CLIENT_LDLIBS) -o $@

$(BIN_DIR)/0pty-server: $(SERVER_OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) $(SERVER_LDLIBS) -o $@

$(SRC_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TEST_BIN): tests/protocol_ring_test.c $(SRC_DIR)/protocol.c $(SRC_DIR)/ring.c | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -pthread -o $@

test: all $(TEST_BIN)
	$(TEST_BIN)

clean:
	rm -rf $(BIN_DIR) $(wildcard $(SRC_DIR)/*.o) $(wildcard $(SRC_DIR)/*.d)

-include $(wildcard $(SRC_DIR)/*.d)

.PHONY: all clean test
