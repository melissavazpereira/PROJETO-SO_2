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
    fprintf(stderr, "Error creating fifo %s: %s", notif_pipe_path, strerror(errno));
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


  return 0;
}

void pacman_play(char command) {

  // TODO - implement me

}

int pacman_disconnect() {
  // TODO - implement me
  return 0;
}

Board receive_board_update(void) {
    // TODO - implement me
}