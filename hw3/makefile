.PHONY: all clear compile run clean d

all: clear compile

clear: 
	@clear

compile: satellite

satellite: satellite.c
	@gcc -Wall -pthread satellite.c -o hw3

d:
	@clear
	@gcc -g -o deb main.c
	@gdb deb

run: satellite
	@./hw3 

clean:
	@rm -f hw3 deb