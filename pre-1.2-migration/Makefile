
BASEDIR	:= $(PWD)
SCRIPT	:= $(BASEDIR)/convert-ignores.pl
TESTDIR	:= $(BASEDIR)/in
CMPDIR	:= $(BASEDIR)/exp
TMPDIR	:= /tmp/fsvs-convert-test.d

MAKEFLAGS	+= --no-print-directory

all: single combined
	@echo All done.

prep:
	@mkdir -p $(TMPDIR)
	@rm -f $(TMPDIR)/* || true
	@cp -a $(TESTDIR)/* $(TMPDIR)/

cmp:
	@rm -f $(TMPDIR)/*.bak
	@diff -au $(TMPDIR)/ $(CMPDIR)

single:
	@$(MAKE) prep
	@echo Single
	@for file in $(wildcard $(TMPDIR)/*) ; do $(SCRIPT) $$file ; done
	@$(MAKE) cmp

combined:
	@$(MAKE) prep
	@echo Combined
	@$(SCRIPT) $(wildcard $(TMPDIR)/*)
	@$(MAKE) cmp

.PHONY: prep cmp single combined
