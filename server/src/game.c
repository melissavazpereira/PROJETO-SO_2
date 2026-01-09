#include "board.h"
#include "utils.h"
#include "protocol.h"
#include "parser.h"
#include "buffer.h"
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
#include <semaphore.h>
#include <signal.h>


// Struct for session data
typedef struct {
    int active; 
    int client_id;
    board_t board;
    int client_req_pipe;
    int client_notif_pipe;
    char client_req_path[MAX_PIPE_PATH_LENGTH];
    char client_notif_path[MAX_PIPE_PATH_LENGTH];
    pthread_t pacman_tid;
    pthread_t session_tid;
    pthread_t *ghost_tids;
    int thread_shutdown;
    pthread_mutex_t session_lock;
    int current_level;
    int total_levels;
    int victory;
    int accumulated_points;
    int level_change_pending;
    int new_level_index;
} session_data_t;


typedef struct {
    board_t *board;
    int ghost_index;
    int *shutdown_flag;
} ghost_thread_arg_t;


static request_buffer_t req_buffer;
static session_data_t *sessions;
static int max_games;
static char *levels_dir;
static pthread_mutex_t sessions_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t sigusr1_received = 0;


void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg; 
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;
    int *shutdown = ghost_arg->shutdown_flag;
    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (1) {
        
        pthread_rwlock_rdlock(&board->state_lock);
        if (*shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL); 
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move % ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
        sleep_ms(board->tempo * (1 + ghost->passo));
    }
}


void* pacman_thread(void *arg) {
    session_data_t *session = (session_data_t*) arg;
    board_t *board = &session->board;
    pacman_t* pacman = &board->pacmans[0];

    while (1) {
        pthread_mutex_lock(&session->session_lock);
        if (!pacman->alive || session->thread_shutdown || session->victory) {
            pthread_mutex_unlock(&session->session_lock);
            pthread_exit(NULL);
        }
        pthread_mutex_unlock(&session->session_lock);

        char op_code;
        ssize_t bytes = read(session->client_req_pipe, &op_code, 1); // Read op code
        
        // Check for read errors
        if (bytes <= 0) {
            pthread_mutex_lock(&session->session_lock);
            session->thread_shutdown = 1;
            pthread_mutex_unlock(&session->session_lock);
            pthread_exit(NULL);
        }

        // Handle disconnect request
        if (op_code == OP_CODE_DISCONNECT) {
            pthread_mutex_lock(&session->session_lock);
            session->thread_shutdown = 1;
            pthread_mutex_unlock(&session->session_lock);
            pthread_exit(NULL);
        }
        
        // Handle play request
        if (op_code == OP_CODE_PLAY) {
            char command;
            // Read command
            if (read(session->client_req_pipe, &command, 1) <= 0) {
                pthread_mutex_lock(&session->session_lock);
                session->thread_shutdown = 1;
                pthread_mutex_unlock(&session->session_lock);
                pthread_exit(NULL);
            }

            // Quit command
            if (command == 'Q') {
                pthread_rwlock_wrlock(&board->state_lock);
                pacman->alive = 0;  // Set pacman as dead
                pthread_rwlock_unlock(&board->state_lock);
                
                continue;
            }

            // Move command
            command_t cmd = {.command = command, .turns = 1}; 
            
            pthread_rwlock_wrlock(&board->state_lock);
            int result = move_pacman(board, 0, &cmd);
            pthread_rwlock_unlock(&board->state_lock);

            // Check if Pacman is dead 
            if (result == DEAD_PACMAN || pacman->alive == 0) {
                sleep_ms(board->tempo); 
                continue;
            }
            
            // Check if reached portal
            if (result == REACHED_PORTAL) {
                sleep_ms(board->tempo);
                pthread_mutex_lock(&session->session_lock);

                session->current_level++; // Increment level
                
                // Check for victory
                if (session->current_level >= session->total_levels) {
                    session->victory = 1;
                    pthread_mutex_unlock(&session->session_lock);
                    sleep_ms(board->tempo);
                    continue; // Exit to notify victory
                } else {
                    session->accumulated_points += board->pacmans[0].points; // Accumulate points
                }
                
                session->level_change_pending = 1; // Request level change
                session->new_level_index = session->current_level; // Set new level index
                
                pthread_mutex_unlock(&session->session_lock);

                sleep_ms(board->tempo);
                
                // Wait until level change is done
                while (1) {
                    pthread_mutex_lock(&session->session_lock);
                    // Check if level change is still pending
                    if (!session->level_change_pending) {
                        pthread_mutex_unlock(&session->session_lock);
                        break;
                    }
                    pthread_mutex_unlock(&session->session_lock);
                    sleep_ms(board->tempo);
                }
                
                sleep_ms(board->tempo);
                continue;
            }

            sleep_ms(board->tempo * (1 + pacman->passo));
        }
    }
}



void* session_manager_thread(void *arg) {
    session_data_t *session = (session_data_t*) arg;
    board_t *board = &session->board;

    while (1) {
        sleep_ms(50); // Fixed bugs

        pthread_mutex_lock(&session->session_lock);
        
        // Check for level change request
        if (session->level_change_pending) {
            int new_level = session->new_level_index; // Get new level index
            
            session->thread_shutdown = 1;
            pthread_mutex_unlock(&session->session_lock);
            
            for (int i = 0; i < board->n_ghosts; i++) {
                pthread_join(session->ghost_tids[i], NULL); // Wait for ghost threads to finish
            }
            free(session->ghost_tids); 
            
            // Unload current level
            pthread_rwlock_wrlock(&board->state_lock);
            unload_level(board);
            pthread_rwlock_unlock(&board->state_lock);
            
            // Load new level
            if (load_sorted_level(board, levels_dir, new_level, 0) != 0) {
                pthread_mutex_lock(&session->session_lock);
                session->thread_shutdown = 1;
                pthread_mutex_unlock(&session->session_lock);
                pthread_exit(NULL);
            }
            
            
            pthread_mutex_lock(&session->session_lock);
            session->thread_shutdown = 0;
            session->ghost_tids = malloc(board->n_ghosts * sizeof(pthread_t));

            // Restart ghost threads
            for (int i = 0; i < board->n_ghosts; i++) {
                ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                arg->board = board;
                arg->ghost_index = i;
                arg->shutdown_flag = &session->thread_shutdown; 
                pthread_create(&session->ghost_tids[i], NULL, ghost_thread, arg); // Start ghost thread
            }
            
            session->level_change_pending = 0;  
            pthread_mutex_unlock(&session->session_lock);
        
            continue; 
        }
        
        // Check for shutdown request
        if (session->thread_shutdown) {
            pthread_mutex_unlock(&session->session_lock);
            pthread_exit(NULL);
        }
        
        // Prepare board data to send
        int victory = session->victory;
        int acc_points = session->accumulated_points;
        pthread_mutex_unlock(&session->session_lock);

        pthread_rwlock_rdlock(&board->state_lock);
        
        char op_code = OP_CODE_BOARD;
        int width = board->width;
        int height = board->height;
        int tempo = board->tempo;
        int game_over = !board->pacmans[0].alive;
        int current_level_points = board->pacmans[0].points;
        int total_points = acc_points + current_level_points;


        char *board_str = get_board_displayed(board); // Get board string
        int board_size = width * height;

        pthread_rwlock_unlock(&board->state_lock);

        // Send board data to client
        int write_failed = 0;
        if (write(session->client_notif_pipe, &op_code, 1) <= 0 ||
            write(session->client_notif_pipe, &width, sizeof(int)) <= 0 ||
            write(session->client_notif_pipe, &height, sizeof(int)) <= 0 ||
            write(session->client_notif_pipe, &tempo, sizeof(int)) <= 0 ||
            write(session->client_notif_pipe, &victory, sizeof(int)) <= 0 ||
            write(session->client_notif_pipe, &game_over, sizeof(int)) <= 0 ||
            write(session->client_notif_pipe, &total_points, sizeof(int)) <= 0 ||
            write(session->client_notif_pipe, board_str, board_size) != board_size) {
            write_failed = 1; 
        }
        
        free(board_str); // Free board string

        // Handle write failure
        if (write_failed) {
            pthread_mutex_lock(&session->session_lock);
            session->thread_shutdown = 1;
            pthread_mutex_unlock(&session->session_lock);
            pthread_exit(NULL);
        }

        // Check for game over or victory to shutdown
        if (game_over || victory) {
            sleep_ms(board->tempo);
            pthread_mutex_lock(&session->session_lock);
            session->thread_shutdown = 1;
            pthread_mutex_unlock(&session->session_lock);
            pthread_exit(NULL);
        }
    }
}


void cleanup_session(session_data_t *session) {
    if (!session->active) return;

    pthread_mutex_lock(&session->session_lock);
    session->thread_shutdown = 1;
    pthread_mutex_unlock(&session->session_lock);

    // Wait for threads to finish
    pthread_join(session->pacman_tid, NULL);
    pthread_join(session->session_tid, NULL);
    
    for (int i = 0; i < session->board.n_ghosts; i++) {
        pthread_join(session->ghost_tids[i], NULL);
    }
    
    free(session->ghost_tids);
    
    // Close pipes
    if (session->client_req_pipe != -1) {
        close(session->client_req_pipe);
        session->client_req_pipe = -1;
    }
    if (session->client_notif_pipe != -1) {
        close(session->client_notif_pipe);
        session->client_notif_pipe = -1;
    }
    
    unload_level(&session->board); // Unload level data
    memset(&session->board, 0, sizeof(board_t)); // Clear board data
    pthread_mutex_destroy(&session->session_lock);
    session->active = 0; // Mark session as inactive
    
}


void* session_worker_thread(void *arg) {
    free(arg);

    sigset_t set; // Create signal set
    sigemptyset(&set); // Initialize empty signal set
    sigaddset(&set, SIGUSR1); // Add SIGUSR1 to the set
    pthread_sigmask(SIG_BLOCK, &set, NULL); // Block SIGUSR1 in this thread


    while (1) {
        // Remove request from buffer and process it
        connection_request_t req = buffer_remove(&req_buffer); 
        

        // Find an available session slot
        pthread_mutex_lock(&sessions_mutex);
        int session_id = -1;
        for (int i = 0; i < max_games; i++) {
            if (!sessions[i].active) {
                session_id = i;
                sessions[i].active = 1;  // Mark session as active
                break;
            }
        }
        pthread_mutex_unlock(&sessions_mutex);

        if (session_id == -1) {
            continue;
        }

        // Initialize session data
        session_data_t *session = &sessions[session_id]; // Get session pointer
        pthread_mutex_init(&session->session_lock, NULL);
        session->client_id = req.client_id;
        session->thread_shutdown = 0;
        session->client_req_pipe = -1;
        session->client_notif_pipe = -1;
        session->current_level = 0;
        session->total_levels = count_levels(levels_dir);
        session->victory = 0;
        session->accumulated_points = 0;
        session->level_change_pending = 0;
        session->new_level_index = 0;   


        // Open the notification pipe to write
        session->client_notif_pipe = open(req.notif_pipe_path, O_WRONLY);
        if (session->client_notif_pipe == -1) {
            pthread_mutex_lock(&sessions_mutex);
            session->active = 0;
            pthread_mutex_unlock(&sessions_mutex);
            pthread_mutex_destroy(&session->session_lock);
            continue;
        }

        
        char resp_op_code = OP_CODE_CONNECT; // Op code for connect response
        char result = 0; // Success when connecting
        
        // Send connection response to client
        if (write(session->client_notif_pipe, &resp_op_code, 1) <= 0 ||
            write(session->client_notif_pipe, &result, 1) <= 0) {
            close(session->client_notif_pipe);
            pthread_mutex_lock(&sessions_mutex);
            session->active = 0;
            pthread_mutex_unlock(&sessions_mutex);
            pthread_mutex_destroy(&session->session_lock);
            continue;
        }

        // Open the request pipe to read
        session->client_req_pipe = open(req.req_pipe_path, O_RDONLY);
        if (session->client_req_pipe == -1) {
            close(session->client_notif_pipe);
            pthread_mutex_lock(&sessions_mutex);
            session->active = 0;
            pthread_mutex_unlock(&sessions_mutex);
            pthread_mutex_destroy(&session->session_lock);
            continue;
        }

        // Save pipe paths
        strncpy(session->client_req_path, req.req_pipe_path, MAX_PIPE_PATH_LENGTH);
        strncpy(session->client_notif_path, req.notif_pipe_path, MAX_PIPE_PATH_LENGTH);

        // Load the first level
        if (load_sorted_level(&session->board, levels_dir, 0, 0) != 0) {
            close(session->client_req_pipe);
            close(session->client_notif_pipe);
            pthread_mutex_lock(&sessions_mutex);
            session->active = 0;
            pthread_mutex_unlock(&sessions_mutex);
            pthread_mutex_destroy(&session->session_lock);
            continue;
        }

        session->ghost_tids = malloc(session->board.n_ghosts * sizeof(pthread_t));

        // Create threads for Pacman, session manager, and ghosts
        pthread_create(&session->pacman_tid, NULL, pacman_thread, session);
        pthread_create(&session->session_tid, NULL, session_manager_thread, session);
        
        for (int i = 0; i < session->board.n_ghosts; i++) {
            ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
            arg->board = &session->board;
            arg->ghost_index = i;
            arg->shutdown_flag = &session->thread_shutdown;
            pthread_create(&session->ghost_tids[i], NULL, ghost_thread, arg);
        }

        // Wait for Pacman thread to finish
        pthread_join(session->pacman_tid, NULL);
        
        cleanup_session(session);
        
        pthread_mutex_lock(&sessions_mutex);
        session->active = 0;
        pthread_mutex_unlock(&sessions_mutex);
    }

    return NULL;
}

// Signal handler for SIGUSR1
void sigusr1_handler(int sig) {
    (void)sig; 
    sigusr1_received = 1; // Set flag to indicate signal received
}

void* host_thread(void *arg) {
    char *fifo_pathname = (char*)arg; 

    sigset_t set; // Create signal set
    sigemptyset(&set); // Initialize empty signal set
    sigaddset(&set, SIGUSR1); // Add SIGUSR1 to the set
    pthread_sigmask(SIG_UNBLOCK, &set, NULL); // Unblock SIGUSR1 in this thread
    
    struct sigaction sa; // Set up signal action
    sa.sa_handler = sigusr1_handler; // Set signal handler with sigusr1_handler
    sigemptyset(&sa.sa_mask); // No additional signals to block
    sa.sa_flags = 0; // No special flags, default behavior
    sigaction(SIGUSR1, &sa, NULL); // Register signal handler


    int server_pipe = open(fifo_pathname, O_RDWR); // Open server FIFO for reading and writing to avoid EOF    
    if (server_pipe == -1) {
        fprintf(stderr, "Error opening server pipe: %s\n", strerror(errno));
        return NULL;
    }


    while (1) {
        // Check if SIGUSR1 was received
        if (sigusr1_received) {
            sigusr1_received = 0;
            
            
            FILE *f = fopen("top5_clients.txt", "w"); // Open file for writing
            if (f) {
                fprintf(f, "Top 5 Clients Connected\n\n"); 
                
                typedef struct { int id; int points; } client_score_t; // Struct for client score
                client_score_t scores[max_games]; // Array to save scores
                int count = 0;
                
                pthread_mutex_lock(&sessions_mutex);
                for (int i = 0; i < max_games; i++) {
                    if (sessions[i].active) {
                        scores[count].id = sessions[i].client_id; // Save client ID
                        // Save total points
                        scores[count].points = sessions[i].accumulated_points + sessions[i].board.pacmans[0].points;
                        count++;
                    }
                }
                pthread_mutex_unlock(&sessions_mutex);
                
                // Sort scores in descending order
                for (int i = 0; i < count - 1; i++) {
                    for (int j = 0; j < count - i - 1; j++) {
                        if (scores[j].points < scores[j + 1].points) {
                            client_score_t temp = scores[j];
                            scores[j] = scores[j + 1];
                            scores[j + 1] = temp;
                        }
                    }
                }
                
                int limit = count < 5 ? count : 5; // Limit to top 5 clients
                for (int i = 0; i < limit; i++) {
                    fprintf(f, "%d. Client ID %d - %d points\n", 
                            i + 1, scores[i].id, scores[i].points); // Write client score to file
                }
                
                if (count == 0) {
                    fprintf(f, "No active clients.\n");
                }
                
                fclose(f);
                printf("Top 5 clients file generated (top5_clients.txt)\n");
            }
        }
        
        char op_code;
        ssize_t bytes = read(server_pipe, &op_code, 1); // Read op code from server pipe
        
        if (bytes <= 0) {
            if (errno == EINTR) continue; // Interrupted by signal, retry
            continue;
        }

        // Process only connection requests
        if (op_code != OP_CODE_CONNECT) {
            continue;
        }    

        // Read connection request data
        connection_request_t req;
        if (read(server_pipe, &req.client_id, sizeof(int)) != sizeof(int) ||
            read(server_pipe, req.req_pipe_path, MAX_PIPE_PATH_LENGTH) != MAX_PIPE_PATH_LENGTH ||
            read(server_pipe, req.notif_pipe_path, MAX_PIPE_PATH_LENGTH) != MAX_PIPE_PATH_LENGTH) {
            continue;
        }
        
        buffer_insert(&req_buffer, req); // Insert request into buffer 
    }

    close(server_pipe);
    return NULL;
}


int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE signals
    if (argc != 4) {
        printf("Usage: %s <levels_dir> <max_games> <register_pipe>\n", argv[0]);
        return -1;
    }

    levels_dir = argv[1];
    max_games = atoi(argv[2]);
    char *fifo_pathname = argv[3];

    if (max_games < 1 || max_games > 100) {
        printf("max_games must be between 1 and 100\n");
        return -1;
    }

    srand(time(NULL)); // Seed random number generator
    open_debug_file("server-debug.log");

    unlink(fifo_pathname); // Remove existing FIFO
    // Create server FIFO
    if (mkfifo(fifo_pathname, 0666) != 0) {
        fprintf(stderr, "Error creating fifo: %s\n", strerror(errno));
        return 1;
    }

    sigset_t set; // Create signal set
    sigemptyset(&set); // Initialize empty signal set
    sigaddset(&set, SIGUSR1); // Add SIGUSR1 to the set
    pthread_sigmask(SIG_BLOCK, &set, NULL); // Block SIGUSR1 in main thread

    buffer_init(&req_buffer); // Initialize request buffer
    sessions = calloc(max_games, sizeof(session_data_t));

    printf("Server initialized\n");

    pthread_t *worker_tids = malloc(max_games * sizeof(pthread_t)); 
    for (int i = 0; i < max_games; i++) {
        int *id = malloc(sizeof(int));
        *id = i;
        pthread_create(&worker_tids[i], NULL, session_worker_thread, id); // Create worker thread
    }

    pthread_t host_tid;
    pthread_create(&host_tid, NULL, host_thread, fifo_pathname); // Create host thread

    pthread_join(host_tid, NULL); // Wait for host thread to finish

    printf("Server shutting down\n");
    
    for (int i = 0; i < max_games; i++) {
        if (sessions[i].active) {
            cleanup_session(&sessions[i]); 
        }
    }

    free(sessions);
    free(worker_tids);
    buffer_destroy(&req_buffer);
    unlink(fifo_pathname);
    close_debug_file();

    return 0;
}