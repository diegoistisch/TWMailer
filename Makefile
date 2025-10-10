all: myfind

myfind: myfind.c
	gcc -Wall -Werror -std=c99 -o myfind myfind.c

clean:
	rm -f myfind