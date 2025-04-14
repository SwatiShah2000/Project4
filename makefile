CC = gcc
CFLAGS = -Wall -g
OBJ = oss.o queue.o user.o

all: oss user

oss: oss.c queue.o
	$(CC) $(CFLAGS) -o oss oss.c queue.o

user: user.c
	$(CC) $(CFLAGS) -o user user.c

queue.o: queue.c queue.h
	$(CC) $(CFLAGS) -c queue.c

clean:
	rm -f *.o oss user msglog.out
