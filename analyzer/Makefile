CC = gcc
CFLAGS = -Wall
SRC = analyzer_tight.c reader.c
TARGET = analyzer_tight

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) -lm

clean:
	rm -f $(TARGET) *.o trigger_log.txt prefetch_log.txt