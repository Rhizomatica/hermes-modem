.PHONY: all modem datalink_arq datalink_broadcast audioio common clean

all: datalink_arq

modem:
	$(MAKE) -C modem

# main is temporarily here, will be moved to a separate directory later
datalink_arq: modem common audioio datalink_broadcast
	$(MAKE) -C datalink_arq

datalink_broadcast: modem common audioio
	$(MAKE) -C datalink_broadcast

audioio: 
	$(MAKE) -C audioio

common:
	$(MAKE) -C common

clean:
	$(MAKE) -C modem clean
	$(MAKE) -C datalink_arq clean
	$(MAKE) -C datalink_broadcast clean
	$(MAKE) -C audioio clean
	$(MAKE) -C common clean
