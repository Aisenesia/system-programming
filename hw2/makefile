.PHONY: all clear compile run clean d

all: clear compile

clear: 
	@clear

compile: 344h2

344h2: main.c
	@gcc main.c -o 344h2

grep:
	@ps aux | grep 344h2

kill:
	@killall -9 344h2

d:
	@clear
	@gcc -g -o deb main.c
	@gdb deb

run: 344h2
	@rm -f fifo1 fifo2
	@./344h2 7 12

clean:
	@rm -f 344h2 deb
	@rm -f fifo1 fifo2

runs:
	./344h2 5 10 & echo $$! > main.pid

signal:
	@if [ -f main.pid ]; then \
		kill -2 `cat main.pid`; \
		echo "Sent SIGINT to main process"; \
	else \
		echo "main.pid file not found. Run 'make run' first."; \
	fi