#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <getopt.h>
#include <unistd.h>
#include <regex.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <pthread.h>
#include "protocol.h"
#include "queue.h"
#include "rwlock.h"
#include "List.h"

int httpRead(char *buffer, intptr_t sock);
void parseRequestLine(const char *requestLine, char *method, char *uri, char *version, int *status);
void parseHeaderLine(const char *header, char *key, char *val, int *status);
void *workerThread(void *jobs);
void httpRespond(int sock, int status);
int get(char *uri, int sock, rwlock_t *rwl);
int put(char *uri, int sock, int contentLength, rwlock_t *rwl);

pthread_mutex_t flockMutex;
List locks;

int main(int argc, char *argv[]) {
    int port;
    int threads = 4;
    int c;

    // Define the valid options and their expected arguments, h for help, t for threads
    static const char *optstring = "ht:";

    while ((c = getopt(argc, argv, optstring)) != -1) {
        switch (c) {
        case 't':
            threads = atoi(optarg);
            if (threads < 0) {
                fprintf(stdout, "Error: Invalid value for thread count. Please provide a "
                                "non-negative number.\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'h': fprintf(stdout, "Usage: ./httpserver [-t threads] <port>\n"); exit(EXIT_SUCCESS);
        case '?':
            fprintf(stdout,
                "Error: Unknown option character '%c'\nUsage: ./httpserver [-t threads] <port>\n",
                optopt);
            exit(EXIT_FAILURE);
        default: fprintf(stdout, "Error parsing options\n"); exit(EXIT_FAILURE);
        }
    }

    // Get port number
    if (optind < argc)
        port = atoi(argv[optind]);
    else {
        fprintf(stdout, "Usage: ./httpserver [-t threads] <port>\n");
        exit(EXIT_FAILURE);
    }

    // Start server event loop
    Listener_Socket sock;
    int sockError = listener_init(&sock, port);
    if (sockError == -1) {
        fprintf(stdout, "Failed to initialize listener socket: Port %d did not respond", port);
        exit(EXIT_FAILURE);
    }

    fprintf(stdout, "==========Starting server event loop==========\n");

    // Set up threads
    pthread_t workerThreads[threads];
    queue_t *jobs = queue_new(threads);

    locks = newList();
    int m = pthread_mutex_init(&flockMutex, NULL);
    if (m != 0) {
        fprintf(stdout, "Mutex Initialization Error!\n");
        exit(EXIT_FAILURE);
    }

    // Create worker threads
    fprintf(stdout, "creating %d worker threads\n", threads);
    fflush(stdout);
    for (int i = 0; i < threads; i++) {
        int res = pthread_create(&workerThreads[i], NULL, workerThread, (void *) jobs);
        if (res != 0) {
            fprintf(stdout, "Error creating worker thread %d\n", i);
            exit(EXIT_FAILURE);
        }
    }

    // Listen for incoming requests and dispatch them to worker threads
    while (1) {
        intptr_t sockFD = listener_accept(&sock);
        if (sockFD == -1) {
            fprintf(stdout, "Failed to accpet socket\n");
            continue;
        }

        // Push the job to the queue
        queue_push(jobs, (void *) sockFD);
    }

    exit(EXIT_SUCCESS);
}

void *workerThread(void *jobs) {
    while (1) {
        intptr_t sock;
        queue_pop(jobs, (void **) &sock);

        char request[256];
        int status = -1;
        char method[8];
        int mode = -1;
        char uri[64];
        char version[10];
        char header[MAX_HEADER_LENGTH];
        char reqKey[256];
        char reqID[256];
        int requestID = -1;
        char contLenKey[64];
        char contLenVal[128];
        int contentLength = -1;

        // Read in request line
        int i = 0;
        for (; i < 256; ++i) {
            int b = read(sock, request + i, 1);
            if (b != 1) {
                fprintf(stdout, "Invalid Command\n");
                status = 400;
                break;
            }

            if (request[i] == '\n') {
                request[i - 1] = '\0';
                break;
            }
        }

        // fprintf(stdout, "Status %d. Read %d bytes out of request line.\n--Request:\n%s\n-------\n",
        //     status, i, request);
        parseRequestLine(request, method, uri, version, &status);
        if (status == -1) {
            // Set Method
            if (strcmp(method, "PUT") == 0)
                mode = 0;
            else if (strcmp(method, "GET") == 0)
                mode = 1;
            else {
                status = 501;
            }

            // Remove leading slash from uri
            for (int i = 0; uri[i] != '\0'; i++) {
                uri[i] = uri[i + 1];
            }

            // Check version
            if (strcmp(version, "HTTP/1.1") != 0) {
                status = 505;
            }
        }

        // Parse headers for request ID
        int s = httpRead(header, sock);
        if (s == -1)
            status = 400;

        parseHeaderLine(header, reqKey, reqID, &status);

        if (strcmp(reqKey, "Request-Id") != 0) {
            fprintf(stdout, "Bad Header\n");
            status = 400;
        } else {
            requestID = atoi(reqID);
        }

        if (mode == 0) {
            // Get content length
            char contentHeader[256];
            s = httpRead(contentHeader, sock);
            if (s == -1) {
                status = 400;
            }
            parseHeaderLine(contentHeader, contLenKey, contLenVal, &status);

            contentLength = atoi(contLenVal);

            // fprintf(stdout, "Working on request %d with content length %d\n", requestID, contentLength);
        }

        // Empty socket
        char temp[64];
        httpRead(temp, sock);

        // Get file lock reference
        pthread_mutex_lock(&flockMutex);

        void *rwl = findLock(locks, uri);
        if (rwl == NULL)
            insert(locks, uri);

        rwl = findLock(locks, uri);
        if (rwl == NULL) {
            fprintf(stdout, "ERROR: COULD NOT GET LOCK FOR %s\n", uri);
            exit(EXIT_FAILURE);
        }
        pthread_mutex_unlock(&flockMutex);

        // Call operation functions
        if (mode == 1 && status == -1) { //GET (reader)
            status = get(uri, sock, rwl);
        } else if (mode == 0 && status == -1) { // PUT (writer)
            status = put(uri, sock, contentLength, rwl);
        }

        httpRespond(sock, status);
        if (status == 200 || status == 201 || status == 404)
            fprintf(stderr, "%s,/%s,%d,%d\n", method, uri, status, requestID);
        fflush(stdout);
    }
}

int httpRead(char *buffer, intptr_t sock) {
    int i = 0;
    for (; i < 2048; ++i) {
        int b = read(sock, buffer + i, 1);
        if (b != 1) {
            fprintf(stdout, "Invalid Header\n");
            return -1;
        }

        if (buffer[i] == '\n') {
            buffer[i + 1] = '\0';
            break;
        }
    }
    return i;
}

void parseRequestLine(const char *req, char *method, char *uri, char *version, int *status) {
    regex_t regex;
    int err;

    char *pattern = "^([A-Z]{1,8}) +(/[a-zA-Z0-9._]{1,63}) +(HTTP/[0-9]\\.[0-9]{1})$";

    err = regcomp(&regex, pattern, REG_EXTENDED);
    if (err != 0) {
        fprintf(stdout, "Internal Servor Error: Request line regcomp error\n");
        *status = 500;
        return;
    }

    regmatch_t pmatch[4];
    size_t nmatch = sizeof(pmatch) / sizeof(pmatch[0]);

    err = regexec(&regex, req, nmatch, pmatch, 0);

    if (err == 0) {
        // Extract captures
        strncpy(method, req + pmatch[1].rm_so, pmatch[1].rm_eo - pmatch[1].rm_so);
        method[pmatch[1].rm_eo - pmatch[1].rm_so] = '\0';

        strncpy(uri, req + pmatch[2].rm_so, pmatch[2].rm_eo - pmatch[2].rm_so);
        uri[pmatch[2].rm_eo - pmatch[2].rm_so] = '\0';

        strncpy(version, req + pmatch[3].rm_so, pmatch[3].rm_eo - pmatch[3].rm_so);
        version[pmatch[3].rm_eo - pmatch[3].rm_so] = '\0';
    } else if (err == REG_NOMATCH) {
        fprintf(stdout, "Bad Request: No regex match\n");
        *status = 400;
        return;
    } else {
        fprintf(stdout, "Internal Server Error: Request Line regexec() error\n");
        *status = 500;
        return;
    }

    regfree(&regex);
    return;
}

void parseHeaderLine(const char *header, char *key, char *val, int *status) {
    regex_t regex;
    int err;

    err = regcomp(&regex, HEADER_FIELD_REGEX, REG_EXTENDED);
    if (err != 0) {
        fprintf(stdout, "Internal Servor Error: Header regcomp error\n");
        *status = 500;
        return;
    }

    regmatch_t matches[3];
    size_t nmatch = sizeof(matches) / sizeof(matches[0]);

    err = regexec(&regex, header, nmatch, matches, 0);
    if (err == 0) {
        // Extract Captures
        strncpy(key, header + matches[1].rm_so, matches[1].rm_eo - matches[1].rm_so);
        key[matches[1].rm_eo - matches[1].rm_so] = '\0';

        strncpy(val, header + matches[2].rm_so, matches[2].rm_eo - matches[2].rm_so);
        val[matches[2].rm_eo - matches[2].rm_so] = '\0';
    } else if (err == REG_NOMATCH) {
        fprintf(stdout, "Bad Request: Bad header\n");
        *status = 400;
        return;
    } else {
        fprintf(stdout, "Internal Server Error: Header regexec() error\n");
        *status = 500;
        return;
    }

    regfree(&regex);
    return;
}

int get(char *uri, int sock, rwlock_t *rwl) {
    char tempBuff[2048];
    // int headerBytesRead;

    // Read rest of socket and ignore any headers
    read_until(sock, tempBuff, 2048, "\r\n\r\n");

    // Acquire lock for file
    reader_lock(rwl);
    int fd = open(uri, O_RDONLY, 0);
    if (fd == -1) {
        reader_unlock(rwl);
        return 404;
    }

    // Get file info
    struct stat fileInfo;
    if (stat(uri, &fileInfo) == -1) {
        fprintf(stdout, "Internal Server Error: Error getting file stats\n");
        close(fd);
        reader_unlock(rwl);
        return 500;
    }

    // Verify given filepath is not a directory
    if (!(S_ISREG(fileInfo.st_mode))) {
        close(fd);
        reader_unlock(rwl);
        return 403;
    }

    // Send the HTTP response headers
    int size = fileInfo.st_size;
    char responseHeader[256];
    snprintf(responseHeader, sizeof(responseHeader),
        "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", size);

    write(sock, responseHeader, strlen(responseHeader));

    // Write file contents in message body
    char fileToSockBuff[2048];
    int bytesRead = 0;
    int totalBytesRead = 0;
    while (totalBytesRead < size) {
        bytesRead = read(fd, fileToSockBuff, sizeof(fileToSockBuff));
        if (bytesRead == 0)
            break;
        totalBytesRead += bytesRead;

        if (bytesRead >= 2048 || bytesRead == size) {
            write(sock, fileToSockBuff, bytesRead); // Write out buffer
            memset(fileToSockBuff, 0, sizeof(fileToSockBuff)); // Empty buffer
            bytesRead = 0;
        }
    }

    // fprintf(stdout, "Read %d bytes in total of %d needed: BR = %d\n", totalBytesRead, size, bytesRead);

    write(sock, fileToSockBuff, bytesRead);

    // Free fd and return success code
    close(fd);
    reader_unlock(rwl);
    return 200;
}

int put(char *filepath, int sock, int contentLength, rwlock_t *rwl) {
    int status;
    // Try to open/create the file and set status accordingly
    status = 500; // Default status
    // Acquire writer lock
    writer_lock(rwl);

    // Attempt to open the file
    int fd = open(filepath, O_TRUNC | O_WRONLY, 0666);

    if (fd == -1) {
        if (errno == ENOENT) {
            // File does not exist, create it
            fd = open(filepath, O_CREAT | O_WRONLY, 0666);
            if (fd != -1) {
                status = 201; // File created
            } else {
                fprintf(stdout, "Error creating file\n");
                writer_unlock(rwl);
                return 500;
            }
        } else {
            fprintf(stdout, "Error opening file\n");
        }
    } else {
        // File opened successfully on first attempt; check if it's a directory
        struct stat fileStat;
        if (fstat(fd, &fileStat) == 0) {
            if (!S_ISDIR(fileStat.st_mode)) {
                // Not a directory
                status = 200; // File exists
            } else {
                fprintf(stdout, "Error: Path is a directory\n");
                close(fd);
                writer_unlock(rwl);
                return 403;
            }
        } else {
            fprintf(stdout, "Error getting file information\n");
            close(fd);
            writer_unlock(rwl);
            return 500;
        }
    }

    // Max size for buffer to read message-body
    char content[4096];

    ssize_t bytesRead = 0;
    ssize_t bytesWritten = 0;
    ssize_t totalBytesWritten = 0;

    while (totalBytesWritten <= contentLength) {
        int b = read(sock, content + bytesRead, sizeof(content));
        if (b <= 0)
            break;

        bytesRead += b;

        if (bytesRead >= 4096 || bytesRead == contentLength) {
            bytesWritten = write(fd, content, bytesRead);

            if (bytesWritten != bytesRead) {
                fprintf(stdout, "Writing Error\n");
                close(fd);
                writer_unlock(rwl);
                return 500;
            }

            totalBytesWritten += bytesWritten;
            bytesRead = 0;
            memset(content, 0, sizeof(content));
        }
    }

    write(fd, content, bytesRead);

    if (status == 200) {
        const char *response = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nOK\n";
        write(sock, response, strlen(response));
    } else if (status == 201) {
        const char *response = "HTTP/1.1 201 Created\r\nContent-Length: 8\r\n\r\nCreated\n";
        write(sock, response, strlen(response));
    }

    close(fd);
    writer_unlock(rwl);
    return status;
}

void httpRespond(int sock, int status) {
    // fprintf(stdout, "httpRespond()\n");
    char temp[512];
    read_until(sock, temp, sizeof(temp), "\r\n");

    const char *responseHeader;
    switch (status) {
    case 200: break;
    case 201: break;
    case 400:
        responseHeader = "HTTP/1.1 400 Bad Request\r\nContent-Length: 12\r\n\r\nBad Request\n";
        write(sock, responseHeader, strlen(responseHeader));
        break;

    case 403:
        responseHeader = "HTTP/1.1 403 Forbidden\r\nContent-Length: 10\r\n\r\nForbidden\n";
        write(sock, responseHeader, strlen(responseHeader));
        break;

    case 404:
        responseHeader = "HTTP/1.1 404 Not Found\r\nContent-Length: 10\r\n\r\nNot Found\n";
        write(sock, responseHeader, strlen(responseHeader));
        break;

    case 500:
        responseHeader = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: "
                         "22\r\n\r\nInternal Server Error\n";
        write(sock, responseHeader, strlen(responseHeader));
        break;

    case 501:
        responseHeader
            = "HTTP/1.1 501 Not Implemented\r\nContent-Length: 16\r\n\r\nNot Implemented\n";
        write(sock, responseHeader, strlen(responseHeader));
        break;

    case 505:
        responseHeader = "HTTP/1.1 505 Version Not Supported\r\nContent-Length: "
                         "22\r\n\r\nVersion Not Supported\n";
        write(sock, responseHeader, strlen(responseHeader));
        break;

    default: break;
    }

    // fprintf(stdout, "------Closing Socket------\n");
    close(sock);
    return;
}
