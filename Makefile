all: client server

client: client.c
	gcc -Wall -Werror -std=c99 -o client client.c

server: server.c
	gcc -Wall -Werror -std=c99 -o server server.c

clean:
	rm -f client server