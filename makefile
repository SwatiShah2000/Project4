CC = gcc
CFLAGS = -Wall -g
OBJECTS = oss.o queue.o

all: oss user

oss: oss.o queue.o
	$(CC) $(CFLAGS) -o oss oss.o queue.o

user: user.o
	$(CC) $(CFLAGS) -o user user.o

oss.o: oss.c queue.h
	$(CC) $(CFLAGS) -c oss.c

user.o: user.c
	$(CC) $(CFLAGS) -c user.c

queue.o: queue.c queue.h
	$(CC) $(CFLAGS) -c queue.c

clean:
	rm -f *.o oss user msglog.out
