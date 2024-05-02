#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: %s <memory_name> <message>\n", argv[0]);
        return 1;
    }

    const char *memory_name = argv[1];
    const char *message = argv[2];

    size_t size = strlen(message) + 1;

    int fd = shm_open(memory_name, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open");
        return 1;
    }

    if (ftruncate(fd, size) == -1) {
        perror("ftruncate");
        return 1;
    }

    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    close(fd);

    memcpy(ptr, message, size);

    printf("Shared memory created with name '%s'\n", memory_name);

    return 0;
}