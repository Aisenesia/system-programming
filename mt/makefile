CC = gcc
CFLAGS = -Wall -g -Iinclude
# -Iinclude is used to include the header files from the include directory
LDFLAGS = -pthread

all: server client teller

server: src/server.c src/common.h
	$(CC) $(CFLAGS) -o server src/server.c $(LDFLAGS)

teller: src/teller.c src/common.h
	$(CC) $(CFLAGS) -o teller src/teller.c $(LDFLAGS)

client: src/client.c src/common.h
	$(CC) $(CFLAGS) -o client src/client.c $(LDFLAGS)

clean:
	ipcrm -a
	rm -f ClientFIFO*
	rm -f ServerFIFO_Name
	rm -f server teller client