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
