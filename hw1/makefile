.PHONY: all clear compile run clean d

all: clear compile

clear: 
	@clear

compile: fileManager

fileManager: main.c
	@gcc main.c -o fileManager

d:
	@clear
	@gcc -g -o deb main.c
	@gdb deb

run: fileManager
	@./fileManager 

clean:
	@rm -f fileManager deb