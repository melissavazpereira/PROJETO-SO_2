CC = gcc
STD_FLAGS = -std=c17 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lncurses -lpthread

C_DIR = client
C_SRC_DIR = $(C_DIR)/src/client
C_OBJ_DIR = $(C_DIR)/obj
C_BIN_DIR = $(C_DIR)/bin
C_INC = -I$(C_DIR)/include -Icommon
C_CFLAGS = $(STD_FLAGS) -g -Wall -Wextra
C_TARGET = $(C_BIN_DIR)/client

S_DIR = server
S_SRC_DIR = $(S_DIR)/src
S_OBJ_DIR = $(S_DIR)/obj
S_BIN_DIR = $(S_DIR)/bin
S_INC = -I$(S_DIR)/include -I$(C_DIR)/include -Icommon
S_CFLAGS = $(STD_FLAGS) -g -Wall -Wextra -Werror
S_TARGET = $(S_BIN_DIR)/Pacmanist

COMMON_DIR = common

C_SRCS = $(wildcard $(C_SRC_DIR)/*.c)
S_SRCS = $(wildcard $(S_SRC_DIR)/*.c) $(wildcard $(COMMON_DIR)/*.c)

OBJS_CLIENT = $(patsubst %.c,$(C_OBJ_DIR)/%,$(notdir $(C_SRCS)))
OBJS_CLIENT := $(OBJS_CLIENT:=.o)

OBJS_SERVER = $(patsubst %.c,$(S_OBJ_DIR)/%,$(notdir $(S_SRCS)))
OBJS_SERVER := $(OBJS_SERVER:=.o)

all: client_build server_build

client_build: folders_client $(C_TARGET)
$(C_TARGET): $(OBJS_CLIENT)
	$(CC) $(C_CFLAGS) $^ -o $@ $(LDFLAGS)

server_build: folders_server $(S_TARGET)
$(S_TARGET): $(OBJS_SERVER)
	$(CC) $(S_CFLAGS) $^ -o $@ $(LDFLAGS)

$(C_OBJ_DIR)/%.o: $(C_SRC_DIR)/%.c
	$(CC) $(C_INC) $(C_CFLAGS) -c $< -o $@

$(S_OBJ_DIR)/%.o: $(S_SRC_DIR)/%.c
	$(CC) $(S_INC) $(S_CFLAGS) -c $< -o $@

$(S_OBJ_DIR)/%.o: $(COMMON_DIR)/%.c
	$(CC) $(S_INC) $(S_CFLAGS) -c $< -o $@

run: server_build
	./$(S_TARGET) $(ARGS)

clean:
	rm -rf $(C_OBJ_DIR)/*.o $(C_BIN_DIR)/client
	rm -rf $(S_OBJ_DIR)/*.o $(S_BIN_DIR)/Pacmanist

folders_client:
	@mkdir -p $(C_OBJ_DIR) $(C_BIN_DIR)

folders_server:
	@mkdir -p $(S_OBJ_DIR) $(S_BIN_DIR)

.PHONY: all client server client_build server_build run clean folders_client folders_server
