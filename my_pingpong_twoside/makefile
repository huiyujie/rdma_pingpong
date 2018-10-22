CFLAGS  := -Wall -Werror -g -std=c99
LD := gcc
LDFLAGS := ${LDFLAGS} -libverbs 

PP: my_pingpong.c
	${LD} -o $@ $^ ${LDFLAGS}

clean: 
	rm -f *.o


