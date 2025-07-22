#include <liburing.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#define QUEUE_DEPTH 256
#define MAX_PATH 4096

struct dir_entry {
  char path[MAX_PATH];
  int level;
  int is_last;
  struct dir_entry *next;
};

struct io_data {
  int op_type; // 0=openat, 1=getdents
  int fd;
  int level;
  char path[MAX_PATH];
  void *buffer;
};

void point_entry(const char *name, int level, int is_last) {
  for (int i = 0; i < level; i++) {
    printf("│   ");
  }
  printf("%s── %s\n", is_last ? "└" : "├", name);
}

int main(int argc, char **argv) {
  struct io_uring ring;
  const char *root = argc > 1 ? argv[1] : ".";

  if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0) {
    perror("queue_init");
    return 1;
  }

  struct dir_entry *queue = malloc(sizeof(struct dir_entry));
  strcpy(queue->path, root);
  queue->level = 0;
  queue->is_last = 1;
  queue->next = NULL;

  printf("%s\n", root);

  while (queue) {
    // batch submit all dirs at current level
    struct dir_entry *current_level = queue;
    struct dir_entry *next_level = NULL;
    struct dir_entry **next_tail = &next_level;

    // submit openat for all dirs at this level
    int submitted = 0;
    for (struct dir_entry *e = current_level; e; e = e->next) {
      struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
      struct io_data *data = malloc(sizeof(struct io_data));

      data->op_type = 0; // openat
      data->level = e->level;
      strcpy(data->path, e->path);

      io_uring_prep_openat(sqe, AT_FDCWD, e->path, O_RDONLY | O_DIRECTORY, 0);
      io_uring_sqe_set_data(sqe, data);
      submitted++;
    }

    io_uring_submit(&ring);

    // process completions
    for (int i = 0; i < submitted; i++) {
      struct io_uring_cqe *cqe;
      io_uring_wait_cqe(&ring, &cqe);

      struct io_data *data = io_uring_cqe_get_data(cqe);

      if (cqe->res >= 0) {
        int fd = cqe->res;

        // read directory entries
        char buf[8192];
        int nread = getdents64(fd, buf, sizeof(buf));

        if (nread > 0) {
          struct dirent64 *d;
          int offset = 0;

          // count entries for is_last determination
          int count = 0;
          while (offset < nread) {
            d = (struct dirent64 *)(buf + offset);
            if (strcmp(d->d_name, ".") && strcmp(d->d_name, "..")) {
              count++;
            }
            offset += d->d_reclen;
          }

          // process entries
          offset = 0;
          int idx = 0;
          while (offset < nread) {
            d = (struct dirent64 *)(buf + offset);

            if (strcmp(d->d_name, ".") && strcmp(d->d_name, "..")) {
              point_entry(d->d_name, data->level + 1, idx == count - 1);

              if (d->d_type == DT_DIR) {
                struct dir_entry *new_entry = malloc(sizeof(struct dir_entry));
                snprintf(new_entry->path, MAX_PATH, "%s/%s", data->path, d->d_name);
                new_entry->level = data->level + 1;
                new_entry->is_last = idx == count - 1;
                new_entry->next = NULL;

                *next_tail = new_entry;
                next_tail = &new_entry->next;
              }
              idx++;
            }
            offset += d->d_reclen;
          }
        }
        close(fd);
      }

      free(data);
      io_uring_cqe_seen(&ring, cqe);
    }

    // move to next level
    struct dir_entry *temp = current_level;
    while (temp) {
      struct dir_entry *next = temp->next;
      free(temp);
      temp = next;
    }
    queue = next_level;
  }

  io_uring_queue_exit(&ring);
  return 0;
}

