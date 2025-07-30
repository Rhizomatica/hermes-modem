.PHONY: all freedv datalink audioio common clean

all: freedv datalink audioio common

freedv:
	$(MAKE) -C freedv

datalink: freedv common audioio
	$(MAKE) -C datalink

audioio: 
	$(MAKE) -C audioio

common:
	$(MAKE) -C common

clean:
	$(MAKE) -C freedv clean
	$(MAKE) -C datalink clean
	$(MAKE) -C audioio clean
	$(MAKE) -C common clean
