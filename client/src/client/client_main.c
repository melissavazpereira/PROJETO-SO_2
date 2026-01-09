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

Board board;
bool stop_execution = false;
int tempo = 0;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static void *receiver_thread(void *arg) {
    (void)arg;
    while (true) {
        Board new_board = receive_board_update();

        // If failed to receive board, terminate
        if (!new_board.data) {
            pthread_mutex_lock(&mutex);
            stop_execution = true; 
            pthread_mutex_unlock(&mutex);
            break;
        }

        pthread_mutex_lock(&mutex);

        // Free previous board data
        if (board.data) 
            free(board.data);

        // Update board and tempo
        board = new_board;
        tempo = new_board.tempo;

        draw_board_client(board);
        refresh_screen();

        // Check for game over or victory to stop execution
        if (board.game_over || board.victory)
            stop_execution = true;
        
        pthread_mutex_unlock(&mutex);

    }
    return NULL;
}

int main(int argc, char *argv[]) {

    // Check the arguments passed on the command line
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Usage: %s <client_id> <register_pipe> [commands_file]\n", argv[0]);
        return 1;
    }

    const char *client_id = argv[1];
    const char *register_pipe = argv[2];
    const char *commands_file = (argc == 4) ? argv[3] : NULL;

    // If commands_file is provided, open it
    int cmd_fd = (commands_file) ? open(commands_file, O_RDONLY) : -1;

    char req_pipe_path[MAX_PIPE_PATH_LENGTH], notif_pipe_path[MAX_PIPE_PATH_LENGTH]; // Paths for the named pipes
    snprintf(req_pipe_path, MAX_PIPE_PATH_LENGTH, "/tmp/%s_request", client_id); // Create request pipe path
    snprintf(notif_pipe_path, MAX_PIPE_PATH_LENGTH, "/tmp/%s_notification", client_id); // Create notification pipe path

    open_debug_file("client-debug.log");
    if (pacman_connect(req_pipe_path, notif_pipe_path, register_pipe) != 0) return 1; // Connect to server

    memset(&board, 0, sizeof(Board)); // Initialize board
    pthread_t receiver_tid; // Thread for receiving board updates
    pthread_create(&receiver_tid, NULL, receiver_thread, NULL); // Create receiver thread

    // Wait until the first board is received
    while (1) {
        pthread_mutex_lock(&mutex);
        int has_board = (board.data != NULL); // Verify if board has been received
        pthread_mutex_unlock(&mutex);
        
        if (has_board) break; // Exit loop if board is received
    }

    terminal_init(); // Initialize terminal for display

    char line_buffer[MAX_COMMAND_LENGTH]; // Buffer for reading commands

    // Loop for sending commands
    while (true) {
        pthread_mutex_lock(&mutex);
        bool should_exit = stop_execution; // Check if execution should stop
        int current_tempo = tempo;  // Get current tempo
        pthread_mutex_unlock(&mutex);

        if (should_exit) break; // Exit loop if game is over or victory

        char command = '\0';

        if (cmd_fd != -1) {
            // Read command from file
            int bytes = read_line(cmd_fd, line_buffer);
            
            if (bytes > 0) {
                // Ignore the comments and empty lines
                if (line_buffer[0] == '#' || line_buffer[0] == '\0') continue;
                // Ignore the "PASSO" and "POS"
                if (strncmp(line_buffer, "PASSO", 5) == 0 || strncmp(line_buffer, "POS", 3) == 0) {
                    continue;
                }

                // Process the command
                char *word = strtok(line_buffer, " \t\n");
                if (!word) continue;

                char cmd_char = toupper(word[0]); // Get the command character

                // Case for 'T' command 
                if (cmd_char == 'T') {
                    char *arg = strtok(NULL, " \t\n");
                    int turns = (arg) ? atoi(arg) : 1; // Transform the argument into integer
                    
                    for (int i = 0; i < turns; i++) {
                        pthread_mutex_lock(&mutex);
                        if (stop_execution) { pthread_mutex_unlock(&mutex); break; }
                        int t = tempo;
                        pthread_mutex_unlock(&mutex);

                        pacman_play('T'); // Send 'T' command to server
                        sleep_ms(t);      // Wait for the tempo
                    }
                    continue; // Continue to next iteration 
                }
                
                command = cmd_char; 
            } else if (bytes == 0) {
                // Rewind the file to read commands again
                lseek(cmd_fd, 0, SEEK_SET); 
                continue;
            }
        } else {
            // Read command from user input
            command = toupper(get_input());
        }

        // If no command, just wait for the tempo
        if (command == '\0') {
            sleep_ms(current_tempo);
            continue;
        }
        
        // Send the command to the server
        pacman_play(command);
        
        // If command is 'Q', set game_over to true and exit loop
        if (command == 'Q') {
            pthread_mutex_lock(&mutex);
            board.game_over = 1;
            pthread_mutex_unlock(&mutex);
            break;
        }
        
        // Wait for the current tempo before next command
        sleep_ms(current_tempo);
    }

    // Disconnect from the server and clean up
    pacman_disconnect();
    pthread_join(receiver_tid, NULL); // Wait for receiver thread to finish

    pthread_mutex_lock(&mutex);
    // If game is over or victory, draw final board 
    if (board.data && (board.game_over || board.victory)) {
        draw_board_client(board);
        refresh_screen();
        pthread_mutex_unlock(&mutex);
        sleep_ms(tempo * 2); 
    } else {
        pthread_mutex_unlock(&mutex);
    }

    if (cmd_fd != -1) close(cmd_fd);
    if (board.data) free(board.data);

    terminal_cleanup();
    close_debug_file();

    return 0;
}