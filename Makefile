CC = g++
CFLAGS = -O2 -g -levent -lpthread

all: ev_server

ev_server: ev_server.cc
	$(CC) $(CFLAGS) ev_server.cc -o ev_server

test:
	./ev_server

clean:
	rm -f ev_server
