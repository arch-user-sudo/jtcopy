# jtcopy
A command line copy tool with a progress bar.

## Usage

```
jtcopy <source> <destination>
```

Copies a file or directory tree the same way `cp` does, with a single-line
progress bar (only when connected to a terminal). Symlinks are recreated (not
followed), and file permissions and timestamps are preserved.

## Build

```
make            # builds ./jtcopy
make install    # installs to /usr/local/bin (override with PREFIX=...)
```

Only the standard C library is required.

This was developed and tested on arch linux. Add the jtcopy file to "/usr/local/bin/" folder to use it. 
I'll post it on the AUR when i feel like its nessesary but for now It stays on github. 

If you've tested this and it works, and maybe you know how to make it a little better, feel free to contribute. 
As for the end users, I've kept it simple and basic so it'll always "Just Work". 

Thanks.

~ Kearan Lynch
