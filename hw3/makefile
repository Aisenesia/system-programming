.PHONY: all clear compile run clean d

all: clear compile

clear: 
	@clear

compile: main

main: main.c
	@gcc -Wall -pthread main.c -o hw3

d:
	@clear
	@gcc -g -o deb main.c
	@gdb deb
v:
	@clear
	@gcc -g -o deb main.c
	@valgrind --leak-check=full ./deb

run: main
	@./hw3 

clean:
	@rm -f hw3 deb