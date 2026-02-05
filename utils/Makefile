# Compiler and flags
CFLAGS += -Wextra

# Source and output files
SRC = broadcast_test.c kiss.c
OUT = broadcast_test

# Default target
all: $(OUT)

# Build the executable
$(OUT): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(OUT) $(SRC)

# Clean up build artifacts
clean:
	rm -f $(OUT)

# Run the program
run: $(OUT)
	./$(OUT) 127.0.0.1 8300

.PHONY: all clean run
