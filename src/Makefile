CC?=gcc

UVDIR=../deps/libuv
HTTPDIR=../deps/http-parser

CFLAGS += -Wall -Wextra -Werror -Wno-unused-parameter  \
          --std=c99 -DPHP_ATOM_INC -DEV_MULTIPLICITY=1 \
          -DPIC -fno-common -fPIC
CFLAGS_FAST = $(CFLAGS) -O2
CFLAGS_DEBUG = $(CFLAGS) -O0 -g

LDFLAGS = -bundle -flat_namespace -undefined suppress

INCLUDES = -I$(UVDIR)/include             \
           -I$(HTTPDIR)                   \
           $(shell php-config --includes)

OBJ = $(UVDIR)/uv.a                       \
      $(HTTPDIR)/libhttp_parser.o         \
      node_events.o                       \
      node_function.o                     \
      node_http.o                         \
      nodephp.o

nodephp.so: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^

$(UVDIR)/uv.a:
	$(MAKE) -C ../deps/libuv uv.a

$(HTTPDIR)/libhttp_parser.o:
	$(MAKE) -C ../deps/http-parser libhttp_parser.o

install:
	cp ./nodephp.so `php-config --extension-dir`/

tag:
	etags *.c *.h

dep:
	$(CC) -MM *.c $(INCLUDES)

clean:
	rm -f *.so *.o
	$(MAKE) -C ../deps/libuv clean
	$(MAKE) -C ../deps/http-parser clean

# orveride the default object rule
%.o:
	$(CC) $(CFLAGS_FAST) $(INCLUDES) -c $*.c -o $@

