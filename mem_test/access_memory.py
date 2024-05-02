import sys
import mmap
import os
from multiprocessing.shared_memory import SharedMemory

# Read shared memory name from command line
if len(sys.argv) < 2:
    print("Usage: python access_memory.py <shared_memory_name>")
    sys.exit(1)

shared_memory_name = sys.argv[1]

# Open shared memory file
try:
    shm = SharedMemory(shared_memory_name, create=False)
except FileNotFoundError as e:
    print(f"Failed to open shared memory: {e}")
    sys.exit(1)

# Read from shared memory
msg = shm._mmap.read()
msg = msg.decode("utf-8")

print("Reading from shared memory: ", msg)

# Close shared memory file
shm.unlink()

# Close shared memory file descriptor
shm.close()
