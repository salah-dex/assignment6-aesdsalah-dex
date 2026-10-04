CC = gcc
CFLAGS = -g -Wall
TARGET = writer
BINARY = *.elf
MAP    = *.map
SOURCES = $(wildcard *.c)  # Use wildcard to find all .c files
OBJECTS = $(SOURCES:.c=.o)  # Convert .c filenames to .o

ifdef CROSS_COMPILE
CC = $(CROSS_COMPILE)gcc
endif

all: $(TARGET)

# Rule to compile object files
# Pattern rule to compile each .c file into a .o file
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Rule to link the target executable
$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS)

clean:
	rm -f $(TARGET) $(OBJECTS) $(MAP) $(BINARY) *.o