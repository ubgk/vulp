#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <memory_name>\n", argv[0]);
        return 1;
    }

    const char *memory_name = argv[1];

    size_t size = 1024;
    int fd = shm_open(memory_name, O_RDWR, 0666);

    if (fd == -1) {
        perror("shm_open");
        return 1;
    }

    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    close(fd);
    
    // Read from shared memory
    printf("Message: %s\n", (char *)ptr);

    // Unlink shared memory
    if (shm_unlink(memory_name) == -1) {
        perror("shm_unlink");
        return 1;
    }

    return 0;
}