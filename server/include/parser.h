#ifndef PARSER_H
#define PARSER_H

#include "board.h"
#define MAX_COMMAND_LENGTH 256

int read_level(board_t* board, char* filename, char* dirname);
int read_pacman(board_t* board, int points);
int read_ghosts(board_t* board);
char** sort_levels(char *levels_dir, int *count_out);
void free_level_names(char **level_names, int count);
int count_levels(char *levels_dir);
int load_sorted_level(board_t *board, char *levels_dir, int level_index, int accumulated_points);

#endif
