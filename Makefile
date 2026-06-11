NAME=bin/storm
CC=gcc
OBJS=$(shell ls src/*.c | sed -e 's/\.c/\.o/' -e 's/src\//bin\//')
CFLAGS=-Wall -Wextra -Wpedantic -O

.PHONY: clean

$(NAME): $(OBJS)
	$(CC) -o $@ $(CFLAGS) $^

bin/%.o: src/%.c src/*.h
	$(CC) -o $@ -c $(CFLAGS) $<

clean:
	rm bin/*.o
