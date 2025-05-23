CC = gcc
CFLAGS = -Wall -Wextra -pthread -Iinclude
SRC = src
OBJ = obj

SRCS_SERVER = $(SRC)/gserver.c
SRCS_CLIENT = $(SRC)/gclient.c

OBJS_SERVER = $(SRCS_SERVER:.c=.o)
OBJS_CLIENT = $(SRCS_CLIENT:.c=.o)

all: server client

server: $(OBJS_SERVER)
	$(CC) $(CFLAGS) -o gserver $(SRCS_SERVER)

client: $(OBJS_CLIENT)
	$(CC) $(CFLAGS) -o gclient $(SRCS_CLIENT)



clean:
	rm -f gserver gclient
	rm -f $(SRC)/*.o

.PHONY: all clean
