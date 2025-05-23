CC = gcc
CFLAGS = -Wall -g -Iinclude
# -Iinclude is used to include the header files from the include directory
LDFLAGS = -lrt

all: server client

server: src/server.c src/common.h
	$(CC) $(CFLAGS) -o server src/server.c src/teller.c $(LDFLAGS)

teller: src/teller.c src/common.h
	$(CC) $(CFLAGS) -o teller src/teller.c $(LDFLAGS)

client: src/client.c src/common.h
	$(CC) $(CFLAGS) -o client src/client.c $(LDFLAGS)

clean:
	ipcrm -a
	rm -f client_fifo*
	rm -f ServerFIFO_Name
	rm -f server teller client
