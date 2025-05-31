CC = gcc
CFLAGS = -Wall -Wextra -pthread -Iinclude
SRC_SERVER = server
SRC_CLIENT = client

OBJ = obj

SRCS_SERVER = $(SRC_SERVER)/gserver.c
SRCS_CLIENT = $(SRC_CLIENT)/gclient.c

OBJS_SERVER = $(SRCS_SERVER:.c=.o)
OBJS_CLIENT = $(SRCS_CLIENT:.c=.o)

all: server client

server: $(OBJS_SERVER)
	$(CC) $(CFLAGS) -o chatserver $(SRCS_SERVER)

client: $(OBJS_CLIENT)
	$(CC) $(CFLAGS) -o chatclient $(SRCS_CLIENT)



clean:
	rm -f chatserver chatclient
	rm -f $(SRC_SERVER)/*.o
	rm -f $(SRC_CLIENT)/*.o

copy:
	cp chatclient test/al/
	cp chatclient test/bob/


.PHONY: all clean
