#ifndef DISPLAY_UTILS_H
#define DISPLAY_UTILS_H

#include "board.h"

/**
 * Returns a string representation of the board for transmission.
 * The caller is responsible for freeing the returned string.
 * 
 * @param board Pointer to the board structure
 * @return Dynamically allocated string with board representation
 */
char* get_board_displayed(board_t* board);

#endif