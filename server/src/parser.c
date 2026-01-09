#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "parser.h"
#include "board.h"
#include "utils.h"
#include <fcntl.h>
#include <dirent.h>
#include <string.h>

int read_level(board_t* board, char* filename, char* dirname) {

    char fullname[MAX_FILENAME];
    strcpy(fullname, dirname);
    strcat(fullname, "/"); 
    strcat(fullname, filename); 

    int fd = open(fullname, O_RDONLY);
    if (fd == -1) {
        debug("Error opening file %s\n", fullname);
        return -1;
    }
    
    char command[MAX_COMMAND_LENGTH];

    // Pacman is optional
    board->pacman_file[0] = '\0';
    board->n_pacmans = 1;

    strcpy(board->level_name, filename);
    *strrchr(board->level_name, '.') = '\0'; // remove .lvl

    int read;
    while ((read = read_line(fd, command)) > 0) {

        // comment
        if (command[0] == '#' || command[0] == '\0') continue;

        char *word = strtok(command, " \t\n");
        if (!word) continue;  // skip empty line

        if (strcmp(word, "DIM") == 0) {
            char *arg1 = strtok(NULL, " \t\n");
            char *arg2 = strtok(NULL, " \t\n");
            if (arg1 && arg2) {
                board->width = atoi(arg1);
                board->height = atoi(arg2);
                debug("DIM = %d x %d\n", board->width, board->height);
            }
        }

        else if (strcmp(word, "TEMPO") == 0) {
            char *arg = strtok(NULL, " \t\n");
            if (arg) {
                board->tempo = atoi(arg);
                debug("TEMPO = %d\n", board->tempo);
            }
        }

        else if (strcmp(word, "PAC") == 0) {
            char *arg = strtok(NULL, " \t\n");
            if (arg) {
                snprintf(board->pacman_file, sizeof(board->pacman_file), "%s/%s", dirname, arg);
                debug("PAC = %s\n", board->pacman_file);
            }
        }

        else if (strcmp(word, "MON") == 0) {
            char *arg;
            int i = 0;
            while ((arg = strtok(NULL, " \t\n")) != NULL) {
                snprintf(board->ghosts_files[i], sizeof(board->ghosts_files[0]), "%s/%s", dirname, arg);
                debug("MON file: %s\n", board->ghosts_files[i]);
                i+= 1;
                if (i == MAX_GHOSTS-1) break;
            }
            board->n_ghosts = i;
        }

        else {
            break;
        }
    }

    if (!board->width || !board->height) {
        debug("Missing dimensions in level file\n");
        close(fd);
        return -1;
    }
    
    // the end of the file contains the grid
    board->board = calloc(board->width * board->height, sizeof(board_pos_t));
    board->pacmans = calloc(board->n_pacmans, sizeof(pacman_t));
    board->ghosts = calloc(board->n_ghosts, sizeof(ghost_t));

    int row = 0;
    // command here still holds the previous line
    while (read > 0) {
        if (command[0]== '#' || command[0] == '\0') continue;
        if (row >= board->height) break;

        debug("Line: %s\n", command);

        for (int col = 0; col < board -> width; col++){
            int idx = row * board->width + col;
            char content = command[col];

            switch (content) {
                case 'X': // wall
                    board->board[idx].content = 'W';
                    break;
                case '@': // portal
                    board->board[idx].content = ' ';
                    board->board[idx].has_portal = 1;
                    break;
                default:
                    board->board[idx].content = ' ';
                    board->board[idx].has_dot = 1;
                    break;
            }
        }

        row++;
        read = read_line(fd, command);
    }

    if (read == -1) {
      debug("Failed parsing line");
      close(fd);
      return read;
    }

    close(fd);
    return 0;
}

int read_pacman(board_t* board, int points) {
    pacman_t* pacman = &board->pacmans[0];

    pacman->alive = 1;
    pacman->points = points;
    pacman->waiting = 0;
    pacman->passo = 0;

    // no file was provided -> defaults
    if (board->pacman_file[0] == '\0') {
        // default position -> find first non occupied cell
        for (int y = 0; y < board->height; y++) {
            for (int x = 0; x < board->width; x++) {
                int idx = y * board->width + x;
                if (board->board[idx].content == ' ') {
                    pacman->pos_x = x;
                    pacman->pos_y = y;
                    board->board[idx].content = 'P';
                    return 0;
                }
            }
        }
        return -1;
    }

    int fd = open(board->pacman_file, O_RDONLY);

    int read;
    char command[MAX_COMMAND_LENGTH];
    while ((read = read_line(fd, command)) > 0) {
        // comment
        if (command[0] == '#' || command[0] == '\0') continue;

        char *word = strtok(command, " \t\n");
        if (!word) continue;  // skip empty line

        if (strcmp(word, "PASSO") == 0) {
            char *arg = strtok(NULL, " \t\n");
            if (arg) {
                pacman->passo = atoi(arg);
                pacman->waiting = pacman->passo;
                debug("Pacman passo: %d\n", pacman->passo);
            }
        }
        else if (strcmp(word, "POS") == 0) {
            char *arg1 = strtok(NULL, " \t\n");
            char *arg2 = strtok(NULL, " \t\n");
            if (arg1 && arg2) {
                pacman->pos_x = atoi(arg1);
                pacman->pos_y = atoi(arg2);
                int idx = pacman->pos_y * board->width + pacman->pos_x;
                board->board[idx].content = 'P';
                debug("Pacman Pos = %d x %d\n", pacman->pos_x, pacman->pos_y);
            }
        }
        else {
            break;
        }
    }

    close(fd);
    return 0;
}


int read_ghosts(board_t* board) {
    for (int i = 0; i < board->n_ghosts; i++) {
        int fd = open(board->ghosts_files[i], O_RDONLY);
        ghost_t* ghost = &board->ghosts[i];

        int read;
        char command[MAX_COMMAND_LENGTH];
        while ((read = read_line(fd, command)) > 0) {
            // comment
            if (command[0] == '#' || command[0] == '\0') continue;

            char *word = strtok(command, " \t\n");
            if (!word) continue;  // skip empty line

            if (strcmp(word, "PASSO") == 0) {
                char *arg = strtok(NULL, " \t\n");
                if (arg) {
                    ghost->passo = atoi(arg);
                    ghost->waiting = ghost->passo;
                    debug("Ghost passo: %d\n", ghost->passo);
                }
            }
            else if (strcmp(word, "POS") == 0) {
                char *arg1 = strtok(NULL, " \t\n");
                char *arg2 = strtok(NULL, " \t\n");
                if (arg1 && arg2) {
                    ghost->pos_x = atoi(arg1);
                    ghost->pos_y = atoi(arg2);
                    int idx = ghost->pos_y * board->width + ghost->pos_x;
                    board->board[idx].content = 'M';
                    debug("Ghost Pos = %d x %d\n", ghost->pos_x, ghost->pos_y);
                }
            }
            else {
                break;
            }
        }

        // end of the file contains the moves
        ghost->current_move = 0;

        // command here still holds the previous line
        int move = 0;
        while (read > 0 && move < MAX_MOVES) {
            if (command[0]== '#' || command[0] == '\0') continue;
            if (command[0] == 'A' ||
                command[0] == 'D' ||
                command[0] == 'W' ||
                command[0] == 'S' ||
                command[0] == 'R' ||
                command[0] == 'C') {
                    ghost->moves[move].command = command[0]; // Add the move to the array
                    ghost->moves[move].turns = 1; // single turn
                    move += 1; // increment move count
            }
            else if (command[0] == 'T' && command[1] == ' ') {
                int t = atoi(command+2);
                if (t > 0) {
                    ghost->moves[move].command = command[0];
                    ghost->moves[move].turns = t; // Set number of turns to wait
                    ghost->moves[move].turns_left = t; // Initialize turns left
                    move += 1; // increment move count
                }
            }
            read = read_line(fd, command);
        }
        ghost->n_moves = move;

        if (read == -1) {
            debug("Failed reading line\n");
            close(fd);
            return -1;
        }
        close(fd);
    }

    return 0;
}


static int compare_strings(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

char** sort_levels(char *levels_dir, int *count_out) {
    DIR* level_dir = opendir(levels_dir);
    if (!level_dir) { 
        *count_out = 0; 
        return NULL; 
    }

    struct dirent* entry;
    int count = 0;

    // Count .lvl files
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

    // Allocate memory in array for level names
    char **level_names = malloc(count * sizeof(char*));
    rewinddir(level_dir); // Go back to the beginning of the directory

    int i = 0;
    // Copy level names
    while ((entry = readdir(level_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".lvl") != 0) continue;
        level_names[i++] = strdup(entry->d_name);
    }

    closedir(level_dir);
    qsort(level_names, count, sizeof(char*), compare_strings); // Sort the level names
    *count_out = count;
    return level_names;
}

void free_level_names(char **level_names, int count) {
    if (!level_names) return;
    for (int i = 0; i < count; i++) free(level_names[i]); // Free each level name
    free(level_names); // Free the array 
}

int count_levels(char *levels_dir) {
    int count;
    char **level_names = sort_levels(levels_dir, &count);
    free_level_names(level_names, count);
    return count;
}

int load_sorted_level(board_t *board, char *levels_dir, int level_index, int accumulated_points) {
    int count;
    char **level_names = sort_levels(levels_dir, &count); // Get sorted level names

    // Check for valid id 
    if (!level_names || level_index < 0 || level_index >= count) {
        free_level_names(level_names, count);
        return -1;
    }

    // Load the specified level
    int result = load_level(board, level_names[level_index], levels_dir, accumulated_points);
    free_level_names(level_names, count);
    return result;
}
