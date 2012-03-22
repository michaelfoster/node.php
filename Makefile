nodephp.so:
	$(MAKE) -C src nodephp.so

install:
	cp src/modules/nodephp.so \
           /usr/local/lib/php/extensions/no-debug-non-zts-20090626/

clean:
	$(MAKE) -C src clean