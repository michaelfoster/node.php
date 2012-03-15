nodephp.so:
	$(MAKE) -C src nodephp.so

install:
	cp src/modules/nodephp.so \
           /usr/local/lib/php/extensions/no-debug-non-zts-20090626/

clean:
	$(MAKE) -C src clean

stop:
	kill `cat /usr/local/var/run/php-fpm.pid`

start:
	/usr/local/sbin/php-fpm
