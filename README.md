# HTTP Server

This directory contains source code for an assignment where I had to implement the functionality of an HTTP server
without the use of higher level libraries.

Compile with `make`. Run using
```bash
./httpserver [-t threads] port
```
## Files
- [httpserver.c](httpserver.c)
    - Main program, handles all the requests and dispatches them to worker threads. Each thread parses the request, processes the file, responds to the connection, and logs the action on its own.
- [List.c](List.c)
    - Custom data structure to store all file names and an associated reader/writer lock to ensure thread safety.
- [List.h](List.h)
    - Defines the API of the List data structure
- [Makefile](Makefile)
    - Compiles and links all program files using make.
    - `make` and `make all` compiles the main program.
    - `make clean` removes all object files and test files
    - `make format` formats all `.c` files and `.h` files
