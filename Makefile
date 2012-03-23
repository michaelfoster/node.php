UVDIR=../deps/libuv
HTTPDIR=../deps/http-parser

nodephp.so: $(UVDIR)/Makefile $(HTTPDIR)/Makefile
	$(MAKE) -C src nodephp.so

install:
	$(MAKE) -C src install

clean:
	$(MAKE) -C src clean

$(UVDIR)/Makefile:
	git submodule update --init $(UVDIR)

$(HTTPDIR)/Makefile:
	git submodule update --init $(HTTPDIR)