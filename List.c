#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "List.h"
#include "rwlock.h"
#include <semaphore.h>

// private Node type
typedef struct NodeObj *Node;

// private NodeObj type
typedef struct NodeObj {
    Node prev;
    ListElement data;
    rwlock_t *fileLock;
    Node next;
} NodeObj;

// private QueueObj type
typedef struct ListObj {
    Node frontDummy;
    Node backDummy;
    int length;
} ListObj;

// newNode()
// Returns reference to new Node object. Initializes next and data fields.
Node newNode(ListElement data) {
    Node N = malloc(sizeof(NodeObj));
    assert(N != NULL);
    N->data = strdup(data);
    N->next = NULL;
    N->prev = NULL;
    N->fileLock = rwlock_new(N_WAY, 1);
    return (N);
}

// freeNode()
// Frees heap memory pointed to by *pN, sets *pN to NULL.
void freeNode(Node *pN) {
    if (pN != NULL && *pN != NULL) {
        rwlock_delete(&(*pN)->fileLock);
        free(*pN);
        *pN = NULL;
    }
}

List newList() {
    List L = calloc(1, sizeof(ListObj));
    if (L == NULL) {
        fprintf(stderr, "List Error: newList(): Failed to initialize file lock list\n");
        exit(EXIT_FAILURE);
    }
    L->frontDummy = newNode("FRONT DUMMY");
    L->backDummy = newNode("BACK DUMMY");
    L->frontDummy->next = L->backDummy;

    L->backDummy->prev = L->frontDummy;

    L->length = 0;

    return L;
}

void freeList(List *pL) {
    if (pL != NULL && *pL != NULL) {
        // Pop all nodes
        Node N = (*pL)->frontDummy->next;
        while (N != (*pL)->backDummy) {
            N = N->next;
            freeNode(&(N->prev));
        }

        freeNode(&(*pL)->frontDummy);
        freeNode(&(*pL)->backDummy);

        free(*pL);
        *pL = NULL;
    }
}

bool insert(List L, ListElement uri) {
    if (L == NULL) {
        fprintf(stderr, "List Error: insert(): NULL List reference\n");
        return false;
    }
    Node N = newNode(uri);
    Node front = L->frontDummy->next;

    front->prev = N;

    L->frontDummy->next = N;
    N->prev = L->frontDummy;

    N->next = front;

    L->length += 1;
    return true;
}

void *findLock(List L, ListElement uri) {
    Node N = L->frontDummy->next;
    for (int i = 0; i < L->length; ++i) {
        if (strcmp(uri, N->data) == 0)
            return N->fileLock;
        N = N->next;
        if (N == L->backDummy)
            break;
    }
    return NULL;
}
