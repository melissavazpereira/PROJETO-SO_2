#include "utils.h"
#include <stdlib.h>
#include <unistd.h>  
#include <sys/types.h>

char* get_board_displayed(board_t* board) {
    size_t buffer_size = (board->width * board->height) + 1;
    char* output = malloc(buffer_size);
    
    if (!output) {
        return NULL;
    }
    
    int pos = 0;
    
    for (int y = 0; y < board->height; y++) {
        for (int x = 0; x < board->width; x++) {
            int index = y * board->width + x;
            char ch = board->board[index].content;
            int ghost_charged = 0;

            // Check if there's a charged ghost at this position
            for (int g = 0; g < board->n_ghosts; g++) {
                ghost_t* ghost = &board->ghosts[g];
                if (ghost->pos_x == x && ghost->pos_y == y) {
                    if (ghost->charged) {
                        ghost_charged = 1;
                    }
                    break;
                }
            }

            // Convert to visual character
            switch (ch) {
                case 'W': // Wall
                    output[pos++] = '#';
                    break;

                case 'P': // Pacman
                    output[pos++] = 'C';
                    break;

                case 'M': // Monster/Ghost
                    if (ghost_charged) {
                        output[pos++] = 'G'; // Charged ghost
                    } else {
                        output[pos++] = 'M'; // Normal ghost
                    }
                    break;

                case ' ': // Empty space
                    if (board->board[index].has_portal) {
                        output[pos++] = '@';
                    }
                    else if (board->board[index].has_dot) {
                        output[pos++] = '.';
                    }
                    else {
                        output[pos++] = ' ';
                    }
                    break;

                default:
                    output[pos++] = ch;
                    break;
            }
        }
    }
    
    output[pos] = '\0';
    return output;
}


int read_line(int fd, char *buf) {
    int i = 0;
    char c;
    ssize_t n;

    while ((n = read(fd, &c, 1)) == 1) {
        if (c == '\r') continue;
        if (c == '\n') break;
        buf[i++] = c;
        if (i == MAX_COMMAND_LENGTH - 1) break;
    }

    buf[i] = '\0';
    if (n == -1) return -1;
    if (n == 0 && i == 0) return 0;
    return i;                         
}