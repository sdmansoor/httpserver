#ifndef LIST_H
#define LIST_H

#include <stdbool.h>
#include <stdio.h>
#include "rwlock.h"
typedef char *ListElement;
typedef struct ListObj *List;

List newList(void); // Creates and returns a new empty List.
void freeList(List *pL); // Frees all heap memory associated with *pL

// Inserts a file into the list and creates its associated file lock
bool insert(List L, ListElement uri);
// Returns the rwlock for the given file
void *findLock(List L, ListElement uri);

#endif
