#include "api.h"
#include "protocol.h"
#include "display.h"
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>

Board global_board;
bool stop_execution = false;
int game_tempo = 0;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static void *receiver_thread(void *arg) {
    (void)arg;
    while (true) {
        Board new_board = receive_board_update();

        if (!new_board.data) {
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            pthread_mutex_unlock(&mutex);
            break;
        }

        pthread_mutex_lock(&mutex);
        if (global_board.data) free(global_board.data);
        global_board = new_board;
        game_tempo = new_board.tempo;

        draw_board_client(global_board);
        refresh_screen();

        if (global_board.game_over || global_board.victory)
            stop_execution = true;
        
        pthread_mutex_unlock(&mutex);

    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Usage: %s <client_id> <register_pipe> [commands_file]\n", argv[0]);
        return 1;
    }

    const char *client_id = argv[1];
    const char *register_pipe = argv[2];
    const char *commands_file = (argc == 4) ? argv[3] : NULL;

    FILE *cmd_fp = (commands_file) ? fopen(commands_file, "r") : NULL;

    char req_pipe_path[MAX_PIPE_PATH_LENGTH], notif_pipe_path[MAX_PIPE_PATH_LENGTH];
    snprintf(req_pipe_path, MAX_PIPE_PATH_LENGTH, "/tmp/%s_request", client_id);
    snprintf(notif_pipe_path, MAX_PIPE_PATH_LENGTH, "/tmp/%s_notification", client_id);

    open_debug_file("client-debug.log");
    if (pacman_connect(req_pipe_path, notif_pipe_path, register_pipe) != 0) return 1;

    memset(&global_board, 0, sizeof(Board));
    pthread_t receiver_tid;
    pthread_create(&receiver_tid, NULL, receiver_thread, NULL);

    // Aguardar receber o primeiro board antes de inicializar o terminal
    while (1) {
        pthread_mutex_lock(&mutex);
        int has_board = (global_board.data != NULL);
        pthread_mutex_unlock(&mutex);
        
        if (has_board) break;
        sleep_ms(50);
    }

    terminal_init();

    while (true) {
        pthread_mutex_lock(&mutex);
        bool should_exit = stop_execution;
        int current_tempo = game_tempo;  // Ler o tempo atual do jogo
        pthread_mutex_unlock(&mutex);

        if (should_exit) break;

        char command = '\0';
        if (cmd_fp) {
            int ch = fgetc(cmd_fp);
            if (ch == EOF) { rewind(cmd_fp); continue; }
            if (ch == 'P') { while (ch != '\n' && ch != EOF) ch = fgetc(cmd_fp); continue; }
            if (isspace(ch)) continue;
            command = toupper((char)ch);
        } else {
            command = toupper(get_input());
        }

        if (command == '\0') {
            // Sem input, aguardar um tick do jogo
            sleep_ms(current_tempo);
            continue;
        }
        
        pacman_play(command);
        
        if (command == 'Q') {
            pthread_mutex_lock(&mutex);
            global_board.game_over = 1;
            pthread_mutex_unlock(&mutex);
            break;
        }
        
        // Aguardar um tick do jogo após processar comando
        sleep_ms(current_tempo);
    }

    pacman_disconnect();
    pthread_join(receiver_tid, NULL);

    // Garantir que a board final é desenhada
    pthread_mutex_lock(&mutex);
    if (global_board.data && (global_board.game_over || global_board.victory)) {
        draw_board_client(global_board);
        refresh_screen();
        pthread_mutex_unlock(&mutex);
        
        // Pausa para ver a mensagem final (2 ticks do jogo)
        sleep_ms(game_tempo * 2); 
    } else {
        pthread_mutex_unlock(&mutex);
    }

    if (cmd_fp) fclose(cmd_fp);
    if (global_board.data) free(global_board.data);

    terminal_cleanup();
    close_debug_file();

    return 0;
}