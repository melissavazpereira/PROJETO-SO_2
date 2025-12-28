CC = gcc
STD_FLAGS = -std=c17 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lncurses -lpthread


C_DIR = client
C_CFLAGS = $(STD_FLAGS) -g -Wall -Wextra
C_INC = -I$(C_DIR)/include -Icommon
C_SRC_DIR = $(C_DIR)/src/client
C_OBJ_DIR = $(C_DIR)/obj
C_BIN_DIR = $(C_DIR)/bin
C_TARGET = $(C_BIN_DIR)/client


S_DIR = server
S_CFLAGS = $(STD_FLAGS) -g -Wall -Wextra -Werror
S_INC = -I$(S_DIR)/include -I$(C_DIR)/include -Icommon
S_SRC_DIR = $(S_DIR)/src
S_OBJ_DIR = $(S_DIR)/obj
S_BIN_DIR = $(S_DIR)/bin
S_TARGET = $(S_BIN_DIR)/Pacmanist


COMMON_DIR = common


OBJS_CLIENT = $(addprefix $(C_OBJ_DIR)/, client_main.o debug.o api.o display.o)
OBJS_SERVER = $(addprefix $(S_OBJ_DIR)/, game.o board.o parser.o display_utils.o)



all: client_build server_build


client: client_build
server: server_build


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


$(S_OBJ_DIR)/board.o: $(S_SRC_DIR)/board.c
	$(CC) $(S_INC) $(S_CFLAGS) -c $< -o $@

$(S_OBJ_DIR)/parser.o: $(S_SRC_DIR)/parser.c
	$(CC) $(S_INC) $(S_CFLAGS) -c $< -o $@

$(S_OBJ_DIR)/display_utils.o: $(COMMON_DIR)/display_utils.c
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