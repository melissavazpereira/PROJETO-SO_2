#include "api.h"
#include "protocol.h"
#include "display.h"
#include "debug.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>

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

    // Abrir ficheiro de comandos se existir (usando open para compatibilidade com read_line)
    int cmd_fd = (commands_file) ? open(commands_file, O_RDONLY) : -1;

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
    }

    terminal_init();

    char line_buffer[MAX_COMMAND_LENGTH];

    while (true) {
        pthread_mutex_lock(&mutex);
        bool should_exit = stop_execution;
        int current_tempo = game_tempo;  // Ler o tempo atual do jogo
        pthread_mutex_unlock(&mutex);

        if (should_exit) break;

        char command = '\0';

        if (cmd_fd != -1) {
            // Leitura usando a sua função read_line
            int bytes = read_line(cmd_fd, line_buffer);
            
            if (bytes > 0) {
                if (line_buffer[0] == '#' || line_buffer[0] == '\0') continue;

                if (strncmp(line_buffer, "PASSO", 5) == 0 || strncmp(line_buffer, "POS", 3) == 0) {
                    continue;
                }

                // 3. Processar o comando
                char *word = strtok(line_buffer, " \t\n");
                if (!word) continue;

                char cmd_char = toupper(word[0]);

                // 4. Tratamento especial para o comando de espera 'T'
                if (cmd_char == 'T') {
                    char *arg = strtok(NULL, " \t\n");
                    int turns = (arg) ? atoi(arg) : 1;
                    
                    for (int i = 0; i < turns; i++) {
                        pthread_mutex_lock(&mutex);
                        if (stop_execution) { pthread_mutex_unlock(&mutex); break; }
                        int t = game_tempo;
                        pthread_mutex_unlock(&mutex);

                        pacman_play('T'); // Envia comando de espera
                        sleep_ms(t);      // Aguarda o tempo do turno
                    }
                    continue; // Avança para a próxima linha do ficheiro
                }
                
                command = cmd_char;
            } else if (bytes == 0) {
                // Fim do ficheiro: volta ao início para repetir movimentos
                lseek(cmd_fd, 0, SEEK_SET);
                continue;
            }
        } else {
            // Se não houver ficheiro, lê do teclado
            command = toupper(get_input());
        }

        // Se não houver comando válido nesta iteração
        if (command == '\0') {
            sleep_ms(current_tempo);
            continue;
        }
        
        // Envia comando (W, A, S, D, R, Q, etc.)
        pacman_play(command);
        
        if (command == 'Q') {
            pthread_mutex_lock(&mutex);
            global_board.game_over = 1;
            pthread_mutex_unlock(&mutex);
            break;
        }
        
        // Aguarda o tempo do jogo antes do próximo comando
        sleep_ms(current_tempo);
    }

    // --- Finalização ---
    pacman_disconnect();
    pthread_join(receiver_tid, NULL);

    pthread_mutex_lock(&mutex);
    if (global_board.data && (global_board.game_over || global_board.victory)) {
        draw_board_client(global_board);
        refresh_screen();
        pthread_mutex_unlock(&mutex);
        sleep_ms(game_tempo * 2); 
    } else {
        pthread_mutex_unlock(&mutex);
    }

    if (cmd_fd != -1) close(cmd_fd);
    if (global_board.data) free(global_board.data);

    terminal_cleanup();
    close_debug_file();

    return 0;
}