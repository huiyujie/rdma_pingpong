CFLAGS  := -Wall -Werror -g
LD := gcc
LDFLAGS := ${LDFLAGS} -libverbs 

pp: my_pingpong.c ib.c sock.c
	${LD} -o $@ $^ ${LDFLAGS}

clean: 
	rm -f *.o


