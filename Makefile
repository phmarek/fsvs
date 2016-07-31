default-target: src/config.h
	@$(MAKE) --no-print-directory -C src

%:
	@$(MAKE) --no-print-directory -C src $@

src/config.h: configure
	@echo ''
	@echo 'You have to run "./configure" before compiling, which might need'
	@echo 'some options depending on your system.'
	@echo ''
	@echo 'See "./configure --help" for a listing of the parameters.'
	@echo ''
	@false

configure:	configure.in
	@echo Generating configure.
	autoconf

distclean:
	rm -f config.cache config.log config.status 2> /dev/null || true
	rm -f src/Makefile src/tags tests/Makefile 2> /dev/null || true
	rm -f src/config.h src/*.[os] src/.*.d src/fsvs 2> /dev/null || true
