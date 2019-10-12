CC=gcc

CFLAGS = -g -c -Wall -pedantic -D_GNU_SOURCE -DLOG_USE_COLOR
#CFLAGS = -ansi -c -Wall -pedantic -D_GNU_SOURCE

all:  mcast start_mcast


bcast: bcast.o
	$(CC) -o bcast bcast.o 

mcast: mcast.o log.o
	$(CC) -o mcast mcast.o log.o

start_mcast: start_mcast.o
	$(CC) -o start_mcast start_mcast.o 

clean:
	rm *.o
	rm mcast
	rm start_mcast