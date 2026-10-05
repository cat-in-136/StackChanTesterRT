# Uncomment lines below if you have problems with $PATH
#SHELL := /bin/bash
#PATH := /usr/local/bin:$(PATH)

subdirs = data/cert

all: compile_commands.json $(subdirs)
	pio -f -c vim run

upload:
	pio -f -c vim run --target upload

clean: $(subdirs)
	rm compile_commands.json
	pio -f -c vim run --target clean

program:
	pio -f -c vim run --target program

uploadfs:
	pio -f -c vim run --target uploadfs

update:
	pio -f -c vim update

compile_commands.json:
	pio -f -c vim run --target compiledb

$(subdirs):
	$(MAKE) -C $@ $(MAKECMDGOALS)

.PHONY: all upload clean program uploadfs update $(subdirs)
