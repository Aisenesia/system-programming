.PHONY: all clear compile run clean d

all: clear compile

clear: 
	@clear

compile: main 

main: 220104004073_main.c buffer.h buffer.c
	@gcc -Wall -pthread 220104004073_main.c buffer.c -o LogAnalyzer

d:
	@clear
	@gcc -g -o deb 220104004073_main.c
	@gdb deb
v:
	@clear
	@gcc -g -o deb 220104004073_main.c
	@valgrind --leak-check=full ./deb

run: main
	@./LogAnalyzer 20 4 /var/log/syslog "ERROR"

clean:
	@rm -f LogAnalyzer deb