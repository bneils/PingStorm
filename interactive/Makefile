NAME=bin/interactive
CC=gcc
OBJS=$(shell ls src/*.c | sed -e 's/\.c/\.o/' -e 's/src\//bin\//')
OBJS+=bin/logging.o
CFLAGS=-Wall -Wextra -O3 -g -lSDL3 -lm

.PHONY: clean

$(NAME): $(OBJS)
	$(CC) -o $@ $(CFLAGS) $^

bin/%.o: src/%.c src/*.h Makefile
	$(CC) -o $@ -c $(CFLAGS) $<

bin/logging.o: ../src/logging.c
	$(CC) -o $@ -c $(CFLAGS) $<

clean:
	rm bin/*.o
