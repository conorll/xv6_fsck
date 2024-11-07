# Makefile for fsck.c

# Compiler
CC = gcc

# Compiler flags
CFLAGS = -Wall -Wextra -g

# Source file
SRC = fsck.c

# Output executable
TARGET = fsck

# Default target
all: $(TARGET)

# Build the target
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

# Clean up
clean:
	rm -f $(TARGET)

# Phony targets
.PHONY: all clean

