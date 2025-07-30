.PHONY: all src datalink audioio common clean

all: src datalink audioio common

src:
	$(MAKE) -C src

datalink: src common audioio
	$(MAKE) -C datalink

audioio: 
	$(MAKE) -C audioio

common:
	$(MAKE) -C common

clean:
	$(MAKE) -C src clean
	$(MAKE) -C datalink clean
	$(MAKE) -C audioio clean
	$(MAKE) -C common clean
