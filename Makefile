CC ?= gcc
VERSION = 1.0.0
DATE = $(shell date +'%d %B %Y')

CFLAGS = -O2 -g -Wall -DSQLITE_HAS_CODEC -I. \
         -DJAILCELL_VERSION=\"$(VERSION)\" \
         -DSQLITE_EXTRA_INIT=sqlcipher_extra_init \
         -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown \
         -DSQLITE_TEMP_STORE=2 \
         -DSQLITE_USE_URI \
         -DSQLITE_DIRECT_OVERFLOW_READ=0
LDFLAGS = -lcrypto -lpthread -ldl -lm -lncursesw

PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man/man1

SQLCIPHER_DIR = deps/sqlcipher-src
SQLCIPHER_ZIP = archive/sqlcipher-5.0.0-beta.zip

CORE_SRCS = main.c cell.c cell.h interface.c interface.h

ALL = jailcell jailcell.1

all: check-deps $(ALL)

check-deps:
	@pkg-config --exists openssl || (echo "Error: openssl development headers missing"; exit 1)
	@pkg-config --exists ncursesw || (echo "Error: ncursesw development headers missing"; exit 1)

jailcell.1: jailcell.1.in
	sed -e 's/@@VERSION@@/$(VERSION)/g' \
	    -e 's/@@DATE@@/$(DATE)/g' < jailcell.1.in > jailcell.1

sqlite3.c sqlite3.h: $(SQLCIPHER_ZIP)
	mkdir -p $(SQLCIPHER_DIR)
	unzip -q -o $(SQLCIPHER_ZIP) -d deps/
	mv deps/sqlcipher-5.0.0-beta/* $(SQLCIPHER_DIR)/ || true
	cd $(SQLCIPHER_DIR) && ./configure --with-tempstore=yes --disable-tcl --disable-shared CFLAGS="-DSQLITE_HAS_CODEC" LDFLAGS="-lcrypto"
	cd $(SQLCIPHER_DIR) && make sqlite3.c
	cp $(SQLCIPHER_DIR)/sqlite3.c .
	cp $(SQLCIPHER_DIR)/sqlite3.h .
	cp $(SQLCIPHER_DIR)/sqlite3ext.h .

cell.o: cell.c cell.h sqlite3.h
	$(CC) $(CFLAGS) -c cell.c -o cell.o

interface.o: interface.c interface.h cell.h sqlite3.h
	$(CC) $(CFLAGS) -c interface.c -o interface.o

sqlite3.o: sqlite3.c sqlite3.h
	$(CC) $(CFLAGS) -c sqlite3.c -o sqlite3.o

jailcell: main.c cell.o interface.o sqlite3.o cJSON.o ezxml.o
	$(CC) $(CFLAGS) main.c cell.o interface.o sqlite3.o cJSON.o ezxml.o -o jailcell $(LDFLAGS)

cJSON.o: cJSON.c cJSON.h
	$(CC) $(CFLAGS) -c cJSON.c -o cJSON.o

ezxml.o: ezxml.c ezxml.h
	$(CC) $(CFLAGS) -c ezxml.c -o ezxml.o

clean:
	rm -f *.o jailcell jailcell.1 sqlite3.c sqlite3.h sqlite3ext.h
	rm -rf deps/sqlcipher-src

install: jailcell jailcell.1
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 jailcell $(DESTDIR)$(BINDIR)/jailcell
	install -d $(DESTDIR)$(MANDIR)
	install -m 644 jailcell.1 $(DESTDIR)$(MANDIR)/jailcell.1

install-strip: jailcell jailcell.1
	install -d $(DESTDIR)$(BINDIR)
	install -s -m 755 jailcell $(DESTDIR)$(BINDIR)/jailcell
	install -d $(DESTDIR)$(MANDIR)
	install -m 644 jailcell.1 $(DESTDIR)$(MANDIR)/jailcell.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/jailcell
	rm -f $(DESTDIR)$(MANDIR)/jailcell.1

strip: jailcell
	strip jailcell

format:
	@for f in $(CORE_SRCS); do \
		unexpand -t 4 --first-only $$f > $$f.tmp && mv $$f.tmp $$f; \
		sed -i 's/[[:blank:]]*$$//' $$f; \
	done

.PHONY: all clean format install install-strip uninstall strip check-deps
