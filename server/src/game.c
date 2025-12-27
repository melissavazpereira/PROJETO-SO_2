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
#include <semaphore.h>

#define BUFFER_SIZE 10

// Estrutura para pedido de conexão
typedef struct {
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
} connection_request_t;

// Buffer produtor-consumidor
typedef struct {
    connection_request_t requests[BUFFER_SIZE];
    int in;
    int out;
    int count;
    pthread_mutex_t mutex;
    sem_t *empty;
    sem_t *full;
} request_buffer_t;

// Estrutura para dados de sessão
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


int count_levels(char *levels_dir);
int load_level_by_index(board_t *board, char *levels_dir, int level_index, int accumulated_points);


// Variáveis globais
static request_buffer_t req_buffer;
static session_data_t *sessions;
static int max_games;
static char *levels_dir;
static pthread_mutex_t sessions_mutex = PTHREAD_MUTEX_INITIALIZER;

// ============================================================================
// BUFFER PRODUTOR-CONSUMIDOR
// ============================================================================

void buffer_init(request_buffer_t *buf) {
    buf->in = 0;
    buf->out = 0;
    buf->count = 0;
    pthread_mutex_init(&buf->mutex, NULL);
    
    char sem_empty_name[64];
    char sem_full_name[64];
    snprintf(sem_empty_name, sizeof(sem_empty_name), "/pacman_empty_%d", getpid());
    snprintf(sem_full_name, sizeof(sem_full_name), "/pacman_full_%d", getpid());
    
    sem_unlink(sem_empty_name);
    sem_unlink(sem_full_name);
    
    buf->empty = sem_open(sem_empty_name, O_CREAT | O_EXCL, 0644, BUFFER_SIZE);
    buf->full = sem_open(sem_full_name, O_CREAT | O_EXCL, 0644, 0);
    
    if (buf->empty == SEM_FAILED || buf->full == SEM_FAILED) {
        perror("sem_open failed");
        exit(1);
    }
}

void buffer_insert(request_buffer_t *buf, connection_request_t req) {
    sem_wait(buf->empty);
    pthread_mutex_lock(&buf->mutex);
    
    buf->requests[buf->in] = req;
    buf->in = (buf->in + 1) % BUFFER_SIZE;
    buf->count++;
    
    pthread_mutex_unlock(&buf->mutex);
    sem_post(buf->full);
}

connection_request_t buffer_remove(request_buffer_t *buf) {
    sem_wait(buf->full);
    pthread_mutex_lock(&buf->mutex);
    
    connection_request_t req = buf->requests[buf->out];
    buf->out = (buf->out + 1) % BUFFER_SIZE;
    buf->count--;
    
    pthread_mutex_unlock(&buf->mutex);
    sem_post(buf->empty);
    
    return req;
}

void buffer_destroy(request_buffer_t *buf) {
    pthread_mutex_destroy(&buf->mutex);
    
    sem_close(buf->empty);
    sem_close(buf->full);
    
    char sem_empty_name[64];
    char sem_full_name[64];
    snprintf(sem_empty_name, sizeof(sem_empty_name), "/pacman_empty_%d", getpid());
    snprintf(sem_full_name, sizeof(sem_full_name), "/pacman_full_%d", getpid());
    
    sem_unlink(sem_empty_name);
    sem_unlink(sem_full_name);
}

// ============================================================================
// THREADS DOS FANTASMAS
// ============================================================================

typedef struct {
    board_t *board;
    int ghost_index;
    int *shutdown_flag;
} ghost_thread_arg_t;

void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;
    int *shutdown = ghost_arg->shutdown_flag;
    free(ghost_arg);

    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (1) {
        sleep_ms(board->tempo * (1 + ghost->passo));

        pthread_rwlock_rdlock(&board->state_lock);
        if (*shutdown) {
            pthread_rwlock_unlock(&board->state_lock);
            pthread_exit(NULL);
        }
        
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move % ghost->n_moves]);
        pthread_rwlock_unlock(&board->state_lock);
    }
}

// ============================================================================
// THREAD DO PACMAN (processa comandos do cliente)
// ============================================================================

void* pacman_thread(void *arg) {
    session_data_t *session = (session_data_t*) arg;
    board_t *board = &session->board;
    pacman_t* pacman = &board->pacmans[0];

    debug("Pacman thread started for client %d\n", session->client_id);

    while (1) {
        pthread_mutex_lock(&session->session_lock);
        if (!pacman->alive || session->thread_shutdown) {
            pthread_mutex_unlock(&session->session_lock);
            debug("Pacman thread exiting for client %d\n", session->client_id);
            pthread_exit(NULL);
        }
        pthread_mutex_unlock(&session->session_lock);

        char op_code;
        ssize_t bytes = read(session->client_req_pipe, &op_code, 1);
        
        if (bytes <= 0) {
            debug("Client %d disconnected (read error)\n", session->client_id);
            pthread_mutex_lock(&session->session_lock);
            session->thread_shutdown = 1;
            pthread_mutex_unlock(&session->session_lock);
            pthread_exit(NULL);
        }

        if (op_code == OP_CODE_DISCONNECT) {
            debug("Client %d requested disconnect\n", session->client_id);
            pthread_mutex_lock(&session->session_lock);
            session->thread_shutdown = 1;
            pthread_mutex_unlock(&session->session_lock);
            pthread_exit(NULL);
        }

        if (op_code == OP_CODE_PLAY) {
            char command;
            if (read(session->client_req_pipe, &command, 1) <= 0) {
                debug("Error reading command from client %d\n", session->client_id);
                pthread_mutex_lock(&session->session_lock);
                session->thread_shutdown = 1;
                pthread_mutex_unlock(&session->session_lock);
                pthread_exit(NULL);
            }
            
            debug("Client %d command: %c\n", session->client_id, command);

            // Tratar comando Q como GAME OVER
            if (command == 'Q') {
                debug("Client %d pressed Q - Game Over\n", session->client_id);
                pthread_rwlock_wrlock(&board->state_lock);
                pacman->alive = 0;  // Marcar como morto para mostrar game over
                pthread_rwlock_unlock(&board->state_lock);
                
                // NÃO sair imediatamente - deixar session_manager enviar updates
                continue;
            }

            command_t cmd = {.command = command, .turns = 1};
            
            pthread_rwlock_wrlock(&board->state_lock);
            int result = move_pacman(board, 0, &cmd);
            pthread_rwlock_unlock(&board->state_lock);

            // Pacman morreu (fantasma matou) - NÃO sair imediatamente
            if (result == DEAD_PACMAN || pacman->alive == 0) {
                sleep_ms(board->tempo);
                debug("Client %d died - Game Over\n", session->client_id);
                // pacman->alive já foi setado a 0 em move_pacman
                // Deixar session_manager_thread enviar updates
                continue;
            }
            
            // Pacman atingiu o portal
            if (result == REACHED_PORTAL) {
                sleep_ms(board->tempo);
                pthread_mutex_lock(&session->session_lock);

                session->current_level++;
                
                debug("Client %d reached portal! Level %d/%d\n", 
                    session->client_id, session->current_level, session->total_levels);
                
                if (session->current_level >= session->total_levels) {
                    session->victory = 1;
                    pthread_mutex_unlock(&session->session_lock);
                    continue;  // Deixar session_manager enviar vitória
                } else {
                    session->accumulated_points += board->pacmans[0].points;
                }
                
                session->level_change_pending = 1;
                session->new_level_index = session->current_level;
                
                pthread_mutex_unlock(&session->session_lock);

                sleep_ms(board->tempo);
                

                while (1) {
                    pthread_mutex_lock(&session->session_lock);
                    if (!session->level_change_pending) {
                        pthread_mutex_unlock(&session->session_lock);
                        break;
                    }
                    pthread_mutex_unlock(&session->session_lock);
                    sleep_ms(board->tempo);
                }
                
                debug("Client %d: Level change complete\n", session->client_id);
                continue;
            }

            sleep_ms(board->tempo * (1 + pacman->passo));
        }
    }
}

// ============================================================================
// THREAD DE ATUALIZAÇÕES (envia estado do tabuleiro periodicamente)
// ============================================================================

void* session_manager_thread(void *arg) {
    session_data_t *session = (session_data_t*) arg;
    board_t *board = &session->board;

    while (1) {
        sleep_ms(50);  // Frequência de atualização

        pthread_mutex_lock(&session->session_lock);
        
        if (session->level_change_pending) {
            int new_level = session->new_level_index;

            
            debug("Session manager: Changing to level %d\n", new_level);
            
            // Parar fantasmas
            session->thread_shutdown = 1;
            pthread_mutex_unlock(&session->session_lock);
            
            for (int i = 0; i < board->n_ghosts; i++) {
                pthread_join(session->ghost_tids[i], NULL);
            }
            free(session->ghost_tids);
            
            // Descarregar nível atual
            pthread_rwlock_wrlock(&board->state_lock);
            unload_level(board);
            pthread_rwlock_unlock(&board->state_lock);
            
            // Carregar novo nível
            if (load_level_by_index(board, levels_dir, new_level, 0) != 0) {
                debug("Session manager: Failed to load level %d\n", new_level);
                pthread_mutex_lock(&session->session_lock);
                session->thread_shutdown = 1;
                pthread_mutex_unlock(&session->session_lock);
                pthread_exit(NULL);
            }
            
            // Reiniciar fantasmas com novo nível
            pthread_mutex_lock(&session->session_lock);
            session->thread_shutdown = 0;
            session->ghost_tids = malloc(board->n_ghosts * sizeof(pthread_t));
            
            for (int i = 0; i < board->n_ghosts; i++) {
                ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
                arg->board = board;
                arg->ghost_index = i;
                arg->shutdown_flag = &session->thread_shutdown;
                pthread_create(&session->ghost_tids[i], NULL, ghost_thread, arg);
            }
            
            session->level_change_pending = 0;  // ✅ Sinalizar conclusão
            pthread_mutex_unlock(&session->session_lock);
            
            debug("Session manager: Level %d loaded successfully\n", new_level);
            continue;
        }
        
        // Verificar se deve terminar
        if (session->thread_shutdown) {
            pthread_mutex_unlock(&session->session_lock);
            pthread_exit(NULL);
        }
        
        int victory = session->victory;
        int acc_points = session->accumulated_points;
        pthread_mutex_unlock(&session->session_lock);

        // Ler estado do tabuleiro
        pthread_rwlock_rdlock(&board->state_lock);
        
        char op_code = OP_CODE_BOARD;
        int width = board->width;
        int height = board->height;
        int tempo = board->tempo;
        int game_over = !board->pacmans[0].alive;
        int current_level_points = board->pacmans[0].points;
        int total_points = acc_points + current_level_points;


        char *board_str = get_board_displayed(board);
        int board_size = width * height;

        pthread_rwlock_unlock(&board->state_lock);

        // Enviar dados ao cliente
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

        free(board_str);

        if (write_failed) {
            debug("Write failed for client %d, exiting\n", session->client_id);
            pthread_mutex_lock(&session->session_lock);
            session->thread_shutdown = 1;
            pthread_mutex_unlock(&session->session_lock);
            pthread_exit(NULL);
        }

        // Se jogo terminou (vitória ou morte), aguardar um pouco e terminar
        if (game_over || victory) {
            sleep_ms(board->tempo);
            pthread_mutex_lock(&session->session_lock);
            session->thread_shutdown = 1;
            pthread_mutex_unlock(&session->session_lock);
            pthread_exit(NULL);
        }
    }
}

// ============================================================================
// GESTÃO DE SESSÕES
// ============================================================================

void cleanup_session(session_data_t *session) {
    if (!session->active) return;

    debug("Cleaning up session for client %d\n", session->client_id);

    pthread_mutex_lock(&session->session_lock);
    session->thread_shutdown = 1;
    pthread_mutex_unlock(&session->session_lock);

    // Aguardar threads
    pthread_join(session->pacman_tid, NULL);
    pthread_join(session->session_tid, NULL);
    
    for (int i = 0; i < session->board.n_ghosts; i++) {
        pthread_join(session->ghost_tids[i], NULL);
    }
    
    free(session->ghost_tids);
    
    // Fechar pipes
    if (session->client_req_pipe != -1) {
        close(session->client_req_pipe);
        session->client_req_pipe = -1;
    }
    if (session->client_notif_pipe != -1) {
        close(session->client_notif_pipe);
        session->client_notif_pipe = -1;
    }
    
    unload_level(&session->board);
    pthread_mutex_destroy(&session->session_lock);
    session->active = 0;
    
    debug("Session cleanup complete for client %d\n", session->client_id);
}

static int compare_strings(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

// Função auxiliar para obter lista ordenada de níveis
static char** get_sorted_levels(char *levels_dir, int *count_out) {
    DIR* level_dir = opendir(levels_dir);
    if (!level_dir) {
        *count_out = 0;
        return NULL;
    }

    // Primeiro, contar quantos níveis existem
    struct dirent* entry;
    int count = 0;
    
    while ((entry = readdir(level_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".lvl") != 0) continue;
        count++;
    }

    if (count == 0) {
        closedir(level_dir);
        *count_out = 0;
        return NULL;
    }

    // Alocar array de strings
    char **level_names = malloc(count * sizeof(char*));
    
    // Voltar ao início do diretório
    rewinddir(level_dir);
    
    // Ler novamente e armazenar os nomes
    int i = 0;
    while ((entry = readdir(level_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".lvl") != 0) continue;
        
        level_names[i] = strdup(entry->d_name);
        i++;
    }

    closedir(level_dir);

    // Ordenar alfabeticamente
    qsort(level_names, count, sizeof(char*), compare_strings);

    *count_out = count;
    return level_names;
}

// Função auxiliar para liberar lista de níveis
static void free_level_names(char **level_names, int count) {
    if (!level_names) return;
    for (int i = 0; i < count; i++) {
        free(level_names[i]);
    }
    free(level_names);
}

// Versão corrigida de count_levels
int count_levels(char *levels_dir) {
    int count;
    char **level_names = get_sorted_levels(levels_dir, &count);
    free_level_names(level_names, count);
    return count;
}

// Versão corrigida de load_level_by_index
int load_level_by_index(board_t *board, char *levels_dir, int level_index, int accumulated_points) {
    int count;
    char **level_names = get_sorted_levels(levels_dir, &count);
    
    if (!level_names || level_index < 0 || level_index >= count) {
        debug("Failed to get sorted levels or invalid index: %d/%d\n", level_index, count);
        free_level_names(level_names, count);
        return -1;
    }

    debug("Loading level %d/%d: %s\n", level_index, count, level_names[level_index]);
    
    int result = load_level(board, level_names[level_index], levels_dir, accumulated_points);
    
    free_level_names(level_names, count);
    
    return result;
}

// ============================================================================
// THREAD TRABALHADORA (processa pedidos de conexão)
// ============================================================================

void* session_worker_thread(void *arg) {
    int worker_id = *(int*)arg;
    free(arg);

    debug("Worker %d started\n", worker_id);

    while (1) {
        // Aguardar pedido de conexão
        connection_request_t req = buffer_remove(&req_buffer);
        
        debug("Worker %d processing connection request\n", worker_id);

        // Encontrar slot livre
        pthread_mutex_lock(&sessions_mutex);
        int session_id = -1;
        for (int i = 0; i < max_games; i++) {
            if (!sessions[i].active) {
                session_id = i;
                sessions[i].active = 1;  // Reservar slot
                break;
            }
        }
        pthread_mutex_unlock(&sessions_mutex);

        if (session_id == -1) {
            debug("Worker %d: No free session slots!\n", worker_id);
            continue;
        }

        session_data_t *session = &sessions[session_id];
        pthread_mutex_init(&session->session_lock, NULL);
        session->client_id = session_id;
        session->thread_shutdown = 0;
        session->client_req_pipe = -1;
        session->client_notif_pipe = -1;
        session->current_level = 0;
        session->total_levels = count_levels(levels_dir);
        session->victory = 0;
        session->accumulated_points = 0;
        session->level_change_pending = 0;
        session->new_level_index = 0;   

        debug("Worker %d: Allocated session %d (total levels: %d)\n", 
              worker_id, session_id, session->total_levels);

        // Abrir pipe de notificações
        session->client_notif_pipe = open(req.notif_pipe_path, O_WRONLY);
        if (session->client_notif_pipe == -1) {
            debug("Worker %d: Error opening client notif pipe: %s\n", 
                  worker_id, strerror(errno));
            pthread_mutex_lock(&sessions_mutex);
            session->active = 0;
            pthread_mutex_unlock(&sessions_mutex);
            pthread_mutex_destroy(&session->session_lock);
            continue;
        }

        // Enviar confirmação de conexão
        char resp_op_code = OP_CODE_CONNECT;
        char result = 0;
        
        if (write(session->client_notif_pipe, &resp_op_code, 1) <= 0 ||
            write(session->client_notif_pipe, &result, 1) <= 0) {
            debug("Worker %d: Error sending connection confirmation\n", worker_id);
            close(session->client_notif_pipe);
            pthread_mutex_lock(&sessions_mutex);
            session->active = 0;
            pthread_mutex_unlock(&sessions_mutex);
            pthread_mutex_destroy(&session->session_lock);
            continue;
        }

        // Abrir pipe de pedidos
        session->client_req_pipe = open(req.req_pipe_path, O_RDONLY);
        if (session->client_req_pipe == -1) {
            debug("Worker %d: Error opening client req pipe: %s\n", 
                  worker_id, strerror(errno));
            close(session->client_notif_pipe);
            pthread_mutex_lock(&sessions_mutex);
            session->active = 0;
            pthread_mutex_unlock(&sessions_mutex);
            pthread_mutex_destroy(&session->session_lock);
            continue;
        }

        strncpy(session->client_req_path, req.req_pipe_path, MAX_PIPE_PATH_LENGTH);
        strncpy(session->client_notif_path, req.notif_pipe_path, MAX_PIPE_PATH_LENGTH);

        // Carregar primeiro nível
        if (load_level_by_index(&session->board, levels_dir, 0, 0) != 0) {
            debug("Worker %d: Failed to load level for session %d\n", 
                  worker_id, session_id);
            close(session->client_req_pipe);
            close(session->client_notif_pipe);
            pthread_mutex_lock(&sessions_mutex);
            session->active = 0;
            pthread_mutex_unlock(&sessions_mutex);
            pthread_mutex_destroy(&session->session_lock);
            continue;
        }

        session->ghost_tids = malloc(session->board.n_ghosts * sizeof(pthread_t));

        debug("Worker %d: Session %d started for client\n", worker_id, session_id);

        // Criar threads da sessão
        pthread_create(&session->pacman_tid, NULL, pacman_thread, session);
        pthread_create(&session->session_tid, NULL, session_manager_thread, session);
        
        for (int i = 0; i < session->board.n_ghosts; i++) {
            ghost_thread_arg_t *arg = malloc(sizeof(ghost_thread_arg_t));
            arg->board = &session->board;
            arg->ghost_index = i;
            arg->shutdown_flag = &session->thread_shutdown;
            pthread_create(&session->ghost_tids[i], NULL, ghost_thread, arg);
        }

        // Aguardar fim da sessão
        pthread_join(session->pacman_tid, NULL);
        
        debug("Worker %d: Session %d finished, cleaning up\n", worker_id, session_id);
        cleanup_session(session);
        
        pthread_mutex_lock(&sessions_mutex);
        session->active = 0;
        pthread_mutex_unlock(&sessions_mutex);
    }

    return NULL;
}

// ============================================================================
// THREAD ANFITRIÃ (recebe pedidos de conexão)
// ============================================================================

void* host_thread(void *arg) {
    char *fifo_pathname = (char*)arg;

    debug("Host thread: Opening server FIFO %s\n", fifo_pathname);

    int server_pipe = open(fifo_pathname, O_RDONLY);
    if (server_pipe == -1) {
        fprintf(stderr, "Error opening server pipe: %s\n", strerror(errno));
        return NULL;
    }

    debug("Host thread: Listening for connections\n");

    while (1) {
        char op_code;
        ssize_t bytes = read(server_pipe, &op_code, 1);
        
        if (bytes <= 0) {
            debug("Host thread: Read error\n");
            continue;
        }

        if (op_code != OP_CODE_CONNECT) {
            debug("Host thread: Invalid opcode: %d\n", op_code);
            continue;
        }

        connection_request_t req;
        if (read(server_pipe, req.req_pipe_path, MAX_PIPE_PATH_LENGTH) != MAX_PIPE_PATH_LENGTH ||
            read(server_pipe, req.notif_pipe_path, MAX_PIPE_PATH_LENGTH) != MAX_PIPE_PATH_LENGTH) {
            debug("Host thread: Error reading connection request\n");
            continue;
        }

        debug("Host thread: New connection request received\n");
        debug("  Req pipe: %s\n", req.req_pipe_path);
        debug("  Notif pipe: %s\n", req.notif_pipe_path);
        
        buffer_insert(&req_buffer, req);
    }

    close(server_pipe);
    return NULL;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char** argv) {
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

    printf("=== PacmanIST Server - Etapa 1.2 ===\n");
    printf("Levels directory: %s\n", levels_dir);
    printf("Max parallel games: %d\n", max_games);
    printf("Register FIFO: %s\n\n", fifo_pathname);

    srand(time(NULL));
    open_debug_file("server-debug.log");

    // Criar FIFO de registro
    unlink(fifo_pathname);
    if (mkfifo(fifo_pathname, 0666) != 0) {
        fprintf(stderr, "Error creating fifo: %s\n", strerror(errno));
        return 1;
    }

    // Inicializar buffer e sessões
    buffer_init(&req_buffer);
    sessions = calloc(max_games, sizeof(session_data_t));

    printf("Server initialized. Waiting for connections...\n\n");

    // Criar threads trabalhadoras
    pthread_t *worker_tids = malloc(max_games * sizeof(pthread_t));
    for (int i = 0; i < max_games; i++) {
        int *id = malloc(sizeof(int));
        *id = i;
        pthread_create(&worker_tids[i], NULL, session_worker_thread, id);
    }

    // Criar thread anfitriã
    pthread_t host_tid;
    pthread_create(&host_tid, NULL, host_thread, fifo_pathname);

    // Aguardar thread anfitriã (servidor nunca termina normalmente)
    pthread_join(host_tid, NULL);

    // Cleanup (nunca alcançado em operação normal)
    printf("Server shutting down...\n");
    
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