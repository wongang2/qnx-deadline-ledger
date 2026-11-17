CC = qcc -Vgcc_ntox86_64
CFLAGS = -O2 -Wall -Wextra -std=gnu11
all: deadlined pumpd supervisor hog dlweb
%: %.c libdeadline.h
	$(CC) $(CFLAGS) -o $@ $<
dlweb_page.inc: dlweb_page.html
	python3 -c 'import sys,json; [print(json.dumps(l)) for l in open(sys.argv[1]).read().splitlines(True)]' $< > $@
dlweb: dlweb.c dlweb_page.inc
	$(CC) $(CFLAGS) -o $@ dlweb.c -lsocket
clean:
	rm -f deadlined pumpd supervisor hog dlweb dlweb_page.inc
