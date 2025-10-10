all: twmailer-client twmailer-server

twmailer-client: twmailer-client.c
	gcc -Wall -Werror -std=c99 -o twmailer-client twmailer-client.c

twmailer-server: twmailer-server.c
	gcc -Wall -Werror -std=c99 -o twmailer-server twmailer-server.c

clean:
	rm -f twmailer-client twmailer-server