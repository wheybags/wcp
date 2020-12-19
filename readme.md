# wcp
wcp is an experiment in re-implementing something like the standard cp file copy tool. The goal is to be as fast as possible, and provide the best possible progress bar, by counting up the total copy size in parallel with running the copy.

Linux only, for now. It should work on any kernel >= 5.6, but I've only tested on 5.8.

## Status
wcp can copy files. Very fast. And show a nice progress bar.
[![asciicast](https://asciinema.org/a/OWNRm4MJdgO1WCvHixughpplO.svg)](https://asciinema.org/a/OWNRm4MJdgO1WCvHixughpplO)

## How fast?
Up to 70% faster than cp, depending on the size of the files being copied. The smaller the files the more dramatic the speedup. It seems to be a good bit faster even for large files.

20 512MiB files:
```
wcp                                 4.33s  2365.12 MiB/s 4.84 files/s
cp -r                               7.86s  1302.92 MiB/s 2.67 files/s
rsync -r --inplace -W --no-compress 16.10s 636.08  MiB/s 1.30 files/s
```

2,000 1MiB files:
```
wcp                                 0.77s 2598.70 MiB/s  2598.70 files/s
cp -r                               1.89s 1058.73 MiB/s  1058.73 files/s
rsync -r --inplace -W --no-compress 3.24s 617.59  MiB/s  617.59  files/s
```

20,000 1KiB files:
```
wcp                                 0.35s 225.71 MiB/s 57145.71 files/s
rsync -r --inplace -W --no-compress 0.83s 95.18  MiB/s 24097.59 files/s
cp -r                               0.94s 84.04  MiB/s 21277.65 files/s
```

## How is it so fast?
I'm using [io_uring](https://kernel.dk/io_uring.pdf), a relatively new IO / syscall system in the linux kernel. It allows you to run system calls asynchronously via a ring buffer in memory shared by user process and kernel, instead of using a full syscall with all of its overhead. I also peg two CPU cores at 100%, allocate a lot of RAM, and the implementation is 100% non-portable. Tradeoffs ;)