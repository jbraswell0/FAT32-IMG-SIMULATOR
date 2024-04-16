CC=gcc
CFLAGS=-Wall -Wextra -g

TARGET=filesys

OBJS=FAT.o

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

FAT.o: FAT.c
	$(CC) $(CFLAGS) -c FAT.c

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
