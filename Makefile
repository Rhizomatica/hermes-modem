.PHONY: all src datalink clean

all: src datalink

src:
	$(MAKE) -C src

datalink: src
	$(MAKE) -C datalink

clean:
	$(MAKE) -C src clean
	$(MAKE) -C datalink clean
