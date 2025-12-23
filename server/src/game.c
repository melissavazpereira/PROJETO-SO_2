#include "board.h"
#include "display_utils.h"
#include "protocol.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include "display.h"

typedef struct {
    board_t *board;
    int ghost_index;
} ghost_thread_arg_t;

typedef struct {
    board_t *board;
    int client_req_pipe;
    int client_notif_pipe;
    char client_req_path[MAX_PIPE_PATH_LENGTH];
    char client_notif_path[MAX_PIPE_PATH_LENGTH];
    int active;
} session_data_t;

int thread_shutdown = 0;
session_data_t session = {0};

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;
    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (true) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move % ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

// Thread que lê comandos do cliente
void* pacman_thread(void *arg) {
    board_t *board = (board_t*) arg;
    pacman_t* pacman = &board->pacmans[0];

    while (true) {
        if (!pacman->alive || thread_shutdown) {
            pthread_exit(NULL);
        }

        // Ler comando do FIFO
        // Formato: (char) OP_CODE=3 | (char) command
        // ou: (char) OP_CODE=2 (disconnect)
        char op_code;
        ssize_t bytes = read(session.client_req_pipe, &op_code, 1);
        
        if (bytes <= 0) {
            debug("Client disconnected (pipe closed)\n");
            pthread_exit(NULL);
        }

        if (op_code == OP_CODE_DISCONNECT) {
            debug("Client requested disconnect\n");
            pthread_exit(NULL);
        }

        if (op_code == OP_CODE_PLAY) {
            char command;
            if (read(session.client_req_pipe, &command, 1) <= 0) {
                debug("Error reading command\n");
                pthread_exit(NULL);
            }
            
            debug("Received command: %c\n", command);

            command_t cmd = {.command = command, .turns = 1};
            
            pthread_rwlock_wrlock(&board->state_lock);
            int result = move_pacman(board, 0, &cmd);
            pthread_rwlock_unlock(&board->state_lock);

            if (result == REACHED_PORTAL || result == DEAD_PACMAN) {
                pthread_exit(NULL);
            }
        }
    }
}

// Thread que envia atualizações periódicas
void* session_manager_thread(void *arg) {
    board_t *board = (board_t*) arg;

    while (true) {
        sleep_ms(board->tempo);

        pthread_rwlock_rdlock(&board->state_lock);
        if (thread_shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }

        // Preparar mensagem de atualização
        // Formato: (char) OP_CODE=4 | (int) width | (int) height | (int) tempo | 
        //          (int) victory | (int) game_over | (int) accumulated_points | 
        //          (char[width * height]) board_data
        
        char op_code = OP_CODE_BOARD;
        int width = board->width;
        int height = board->height;
        int tempo = board->tempo;
        int victory = 0;  // TODO: implementar lógica de vitória
        int game_over = !board->pacmans[0].alive;
        int accumulated_points = board->pacmans[0].points;

        // Obter representação do tabuleiro
        char *board_str = get_board_displayed(board);
        int board_size = width * height;

        pthread_rwlock_unlock(&board->state_lock);

        // Enviar dados sequencialmente
        if (write(session.client_notif_pipe, &op_code, 1) <= 0) {
            debug("Failed to send op_code\n");
            free(board_str);
            pthread_exit(NULL);
        }

        if (write(session.client_notif_pipe, &width, sizeof(int)) <= 0) {
            debug("Failed to send width\n");
            free(board_str);
            pthread_exit(NULL);
        }

        if (write(session.client_notif_pipe, &height, sizeof(int)) <= 0) {
            debug("Failed to send height\n");
            free(board_str);
            pthread_exit(NULL);
        }

        if (write(session.client_notif_pipe, &tempo, sizeof(int)) <= 0) {
            debug("Failed to send tempo\n");
            free(board_str);
            pthread_exit(NULL);
        }

        if (write(session.client_notif_pipe, &victory, sizeof(int)) <= 0) {
            debug("Failed to send victory\n");
            free(board_str);
            pthread_exit(NULL);
        }

        if (write(session.client_notif_pipe, &game_over, sizeof(int)) <= 0) {
            debug("Failed to send game_over\n");
            free(board_str);
            pthread_exit(NULL);
        }

        if (write(session.client_notif_pipe, &accumulated_points, sizeof(int)) <= 0) {
            debug("Failed to send points\n");
            free(board_str);
            pthread_exit(NULL);
        }

        if (write(session.client_notif_pipe, board_str, board_size) != board_size) {
            debug("Failed to send board data\n");
            free(board_str);
            pthread_exit(NULL);
        }

        free(board_str);

        if (game_over) {
            sleep_ms(board->tempo * 2);
            pthread_exit(NULL);
        }
    }
    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: %s <levels_dir> <max_games> <register_pipe>\n", argv[0]);
        return -1;
    }

    char *levels_dir = argv[1];
    int max_games = atoi(argv[2]);
    char *fifo_pathname = argv[3];

    if (max_games != 1) {
        printf("Etapa 1.1: only max_games=1 supported\n");
        return -1;
    }

    // Criar FIFO de registro
    unlink(fifo_pathname);
    
    if (mkfifo(fifo_pathname, 0666) != 0) {
        fprintf(stderr, "Error creating fifo: %s\n", strerror(errno));
        return 1;
    }

    printf("Server waiting on %s...\n", fifo_pathname);

    // Aguardar cliente
    int server_pipe = open(fifo_pathname, O_RDONLY);
    if (server_pipe == -1) {
        fprintf(stderr, "Error opening server pipe: %s\n", strerror(errno));
        return 1;
    }

    // Ler pedido de conexão
    // Formato: (char) OP_CODE=1 | (char[40]) req_pipe | (char[40]) notif_pipe
    char op_code;
    char req_pipe_path[MAX_PIPE_PATH_LENGTH] = {0};
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH] = {0};

    if (read(server_pipe, &op_code, 1) <= 0) {
        fprintf(stderr, "Error reading op_code\n");
        close(server_pipe);
        return 1;
    }

    if (op_code != OP_CODE_CONNECT) {
        printf("Expected connect message, got %d\n", op_code);
        close(server_pipe);
        return 1;
    }

    if (read(server_pipe, req_pipe_path, MAX_PIPE_PATH_LENGTH) != MAX_PIPE_PATH_LENGTH) {
        fprintf(stderr, "Error reading req_pipe_path\n");
        close(server_pipe);
        return 1;
    }

    if (read(server_pipe, notif_pipe_path, MAX_PIPE_PATH_LENGTH) != MAX_PIPE_PATH_LENGTH) {
        fprintf(stderr, "Error reading notif_pipe_path\n");
        close(server_pipe);
        return 1;
    }

    printf("Client connecting...\n");
    printf("  Request pipe: %s\n", req_pipe_path);
    printf("  Notification pipe: %s\n", notif_pipe_path);

    // Abrir pipes do cliente
    session.client_notif_pipe = open(notif_pipe_path, O_WRONLY);
    if (session.client_notif_pipe == -1) {
        fprintf(stderr, "Error opening client notif pipe: %s\n", strerror(errno));
        close(server_pipe);
        return 1;
    }

    // Enviar confirmação
    // Formato: (char) OP_CODE=1 | (char) result
    char resp_op_code = OP_CODE_CONNECT;
    char result = 0;  // 0 = sucesso
    
    if (write(session.client_notif_pipe, &resp_op_code, 1) <= 0) {
        fprintf(stderr, "Error sending response op_code\n");
        close(session.client_notif_pipe);
        close(server_pipe);
        return 1;
    }

    if (write(session.client_notif_pipe, &result, 1) <= 0) {
        fprintf(stderr, "Error sending response result\n");
        close(session.client_notif_pipe);
        close(server_pipe);
        return 1;
    }

    session.client_req_pipe = open(req_pipe_path, O_RDONLY);
    if (session.client_req_pipe == -1) {
        fprintf(stderr, "Error opening client req pipe: %s\n", strerror(errno));
        close(session.client_notif_pipe);
        close(server_pipe);
        return 1;
    }

    strncpy(session.client_req_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
    strncpy(session.client_notif_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
    session.active = 1;

    printf("Client connected successfully!\n");

    srand(time(NULL));
    open_debug_file("server-debug.log");

    DIR* level_dir = opendir(levels_dir);
    if (!level_dir) {
        fprintf(stderr, "Failed to open directory: %s\n", levels_dir);
        return 1;
    }

    board_t game_board;
    struct dirent* entry;
    int level_count = 0;
    
    while ((entry = readdir(level_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".lvl") != 0) continue;

        printf("Loading level: %s\n", entry->d_name);
        load_level(&game_board, entry->d_name, levels_dir, 0);
        session.board = &game_board;

        pthread_t pacman_tid, session_tid;
        pthread_t *ghost_tids = malloc(game_board.n_ghosts * sizeof(pthread_t));
        thread_shutdown = 0;

        debug("Creating threads for level %s\n", entry->d_name);

        pthread_create(&pacman_tid, NULL, pacman_thread, &game_board);
        pthread_create(&session_tid, NULL, session_manager_thread, &game_board);
        
        for (int i = 0; i < game_board.n_ghosts; i++) {
            ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
            arg->board = &game_board;
            arg->ghost_index = i;
            pthread_create(&ghost_tids[i], NULL, ghost_thread, arg);
        }

        pthread_join(pacman_tid, NULL);
        
        pthread_rwlock_wrlock(&game_board.state_lock);
        thread_shutdown = 1;
        pthread_rwlock_unlock(&game_board.state_lock);

        pthread_join(session_tid, NULL);
        for (int i = 0; i < game_board.n_ghosts; i++) {
            pthread_join(ghost_tids[i], NULL);
        }

        free(ghost_tids);
        print_board(&game_board);
        unload_level(&game_board);
        
        level_count++;
        if (level_count >= 1) break; // Etapa 1.1: apenas 1 nível
    }

    close(session.client_req_pipe);
    close(session.client_notif_pipe);
    close(server_pipe);
    unlink(fifo_pathname);
    closedir(level_dir);
    close_debug_file();

    printf("Server shutdown\n");
    return 0;
}