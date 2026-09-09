CC = cc
CFLAGS = -O2 -Wall -Wextra -std=c11

all: test_journal

journal.o: journal.c
	$(CC) $(CFLAGS) -c journal.c -o journal.o

test_journal.o: test_journal.c
	$(CC) $(CFLAGS) -c test_journal.c -o test_journal.o

test_journal: journal.o test_journal.o
	$(CC) $(CFLAGS) journal.o test_journal.o -o test_journal

clean:
	rm -f test_journal *.o vsfs.img
