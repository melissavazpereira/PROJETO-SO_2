#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>


struct Session {
  int id;
  int req_pipe;
  int notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1};

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {

  // Removes req_pipe_path if it exists
  if (unlink(req_pipe_path) != 0 && errno != ENOENT) {
    fprintf(stderr, "Error removing fifo %s: %s\n", req_pipe_path, strerror(errno));
    return 1;
  }

  // Removes notif_pipe_path if it exists
  if (unlink(notif_pipe_path) != 0 && errno != ENOENT) {
    fprintf(stderr, "Error removing fifo %s: %s\n", notif_pipe_path, strerror(errno));
    return 1;
  }

  // Creates req_pipe_path pipe
  if (mkfifo(req_pipe_path, 0666) != 0) {
    fprintf(stderr, "Error creating fifo %s: %s\n", req_pipe_path, strerror(errno));
    return 1;
  }

  // Creates notif_pipe_path pipe
  if (mkfifo(notif_pipe_path, 0666) != 0) {
    fprintf(stderr, "Error creating fifo %s: %s\n", notif_pipe_path, strerror(errno));
    return 1;
  }

  strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);

  // Opens pipe for writing the two named pipes
  int server_pipe = open(server_pipe_path, O_WRONLY);
  if (server_pipe == -1) {
    fprintf(stderr, "Error opening: %s\n", strerror(errno));
    unlink(req_pipe_path);
    unlink(notif_pipe_path);
    return 1;
  }


  char op_code = OP_CODE_CONNECT;
  char req_path_buffer[MAX_PIPE_PATH_LENGTH] = {0};
  char notif_path_buffer[MAX_PIPE_PATH_LENGTH] = {0};

  strncpy(req_path_buffer, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  strncpy(notif_path_buffer, notif_pipe_path, MAX_PIPE_PATH_LENGTH);

  write(server_pipe, &op_code, sizeof(char));
  write(server_pipe, req_path_buffer, MAX_PIPE_PATH_LENGTH);
  write(server_pipe, notif_path_buffer, MAX_PIPE_PATH_LENGTH);


  close(server_pipe);

  session.notif_pipe = open(notif_pipe_path, O_RDONLY);
  if (session.notif_pipe == -1) {
    fprintf(stderr, "Error opening: %s\n", strerror(errno));
    unlink(req_pipe_path);
    unlink(notif_pipe_path);
    return 1;
  }

  char resp_op_code;
  char result;

  read(session.notif_pipe, &resp_op_code, sizeof(char));
  read(session.notif_pipe, &result, sizeof(char));

  if (resp_op_code != OP_CODE_CONNECT || result != 0) {
    fprintf(stderr, "Connection refused: %s\n", strerror(errno));
    close(session.notif_pipe);
    unlink(req_pipe_path);
    unlink(notif_pipe_path);
    return 1;
  }

  session.req_pipe = open(req_pipe_path, O_WRONLY);
  if (session.req_pipe == -1) {
    fprintf(stderr, "Error opening: %s\n", strerror(errno));
    close(session.notif_pipe);
    unlink(req_pipe_path);
    unlink(notif_pipe_path);
    return 1;
  }

  return 0;
}

void pacman_play(char command) {

  if (session.req_pipe == -1) {
    return;
  }

  char message[2];

  message[0] = OP_CODE_PLAY;
  message[1] = command;

  if (write(session.req_pipe, message, 2) != 2) {
    fprintf(stderr, "Error sending play command: %s\n", strerror(errno));
  }

}


int pacman_disconnect() {
  if (session.req_pipe == -1) {
    return -1;
  }

  char message = OP_CODE_DISCONNECT;
  write(session.req_pipe, &message, 1);


  // Close pipes
  if (session.req_pipe != -1) {
    close(session.req_pipe);
  }

  if(session.notif_pipe != -1) {
    close(session.notif_pipe);
  }

  unlink(session.req_pipe_path);
  unlink(session.notif_pipe_path);

  session.req_pipe = -1;

  return 0;
}

Board receive_board_update(void) {
    Board board = {0};
    char op_code;

    int n = read(session.notif_pipe, &op_code, 1);

    if (n <= 0 || op_code != OP_CODE_BOARD) {
      board.data = NULL;
      return board;
    }

    read(session.notif_pipe, &board.width, sizeof(int));
    read(session.notif_pipe, &board.height, sizeof(int));
    read(session.notif_pipe, &board.tempo, sizeof(int));
    read(session.notif_pipe, &board.victory, sizeof(int));
    read(session.notif_pipe, &board.game_over, sizeof(int));
    read(session.notif_pipe, &board.accumulated_points, sizeof(int));

    int data_size = board.width * board.height;
    board.data = malloc(data_size * sizeof(char)); 

    if (board.data != NULL) {
      int r = read(session.notif_pipe, board.data, data_size);
      if (r != data_size) {
        free(board.data);
        board.data = NULL;
      }
    }

    return board;
}