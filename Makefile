OS=$(shell uname)

PROG=   mongovi

COMPAT=""

ifeq (${OS},Linux)
COMPAT=strlcat.o strlcpy.o reallocarray.o
endif

ifeq (${OS},Darwin)
COMPAT=reallocarray.o
endif

ifndef USRDIR
  USRDIR=  /usr/local
endif
BINDIR=  $(USRDIR)/bin
MANDIR=  $(USRDIR)/share/man

INCDIR=-I$(DESTDIR)/usr/include/libbson-1.0/ -I$(DESTDIR)/usr/include/libmongoc-1.0/ -I$(DESTDIR)/usr/local/include/libbson-1.0/ -I$(DESTDIR)/usr/local/include/libmongoc-1.0/

CFLAGS=-Wall -Wextra -pedantic -g ${INCDIR}
LDFLAGS=-lmongoc-1.0 -lbson-1.0 -ledit
OBJ=jsmn.o jsonify.o main.o mongovi.o shorten.o prefix_match.o

INSTALL_DIR=  install -dm 755
INSTALL_BIN=  install -m 555
INSTALL_MAN=  install -m 444

${PROG}: ${OBJ} ${COMPAT}
	$(CC) ${CFLAGS} -o $@ ${OBJ} ${COMPAT} ${LDFLAGS}

%.o: %.c
	$(CC) ${CFLAGS} -c $<

%.o: compat/%.c
	$(CC) ${CFLAGS} -c $<

%.o: test/%.c
	$(CC) ${CFLAGS} -c $<

test: test/parse_path.c ${OBJ} ${COMPAT}
	$(CC) $(CFLAGS) mongovi.c prefix_match.c test/parse_path.c -o mongovi-test jsmn.o jsonify.o shorten.o ${COMPAT} ${LDFLAGS}
	./mongovi-test

test-dep:
	$(CC) $(CFLAGS) shorten.c test/shorten.c -o shorten-test
	./shorten-test
	$(CC) $(CFLAGS) prefix_match.c compat/reallocarray.c test/prefix_match.c -o prefix_match-test
	./prefix_match-test

install:
	${INSTALL_DIR} ${DESTDIR}${BINDIR}
	${INSTALL_DIR} ${DESTDIR}${MANDIR}/man1
	${INSTALL_BIN} ${PROG} ${DESTDIR}${BINDIR}
	${INSTALL_MAN} ${PROG}.1 ${DESTDIR}${MANDIR}/man1

depend:
	$(CC) ${CFLAGS} -E -MM *.c > .depend

.PHONY: clean 
clean:
	rm -f ${OBJ} ${COMPAT} mongovi shorten-test prefix_match-test mongovi-test
