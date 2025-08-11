.PHONY: all modem/freedv datalink audioio common clean

all: modem/freedv datalink audioio common

modem/freedv:
	$(MAKE) -C modem/freedv

datalink: modem/freedv common audioio
	$(MAKE) -C datalink

audioio: 
	$(MAKE) -C audioio

common:
	$(MAKE) -C common

clean:
	$(MAKE) -C modem/freedv clean
	$(MAKE) -C datalink clean
	$(MAKE) -C audioio clean
	$(MAKE) -C common clean
