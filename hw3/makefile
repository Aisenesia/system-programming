.PHONY: all clear compile run clean d

all: clear compile

clear: 
	@clear

compile: satellite

satellite: satellite.c
	@gcc -Wall -pthread satellite.c -o hw3

satellite2: sat2.c
	@gcc sat2.c -o satellite

d:
	@clear
	@gcc -g -o deb main.c
	@gdb deb

run: satellite
	@./satellite 

clean:
	@rm -f satellite deb