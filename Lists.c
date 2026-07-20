//
// Created by lilyk on 7/14/2026.
//
/* ============================================================================
 *  lists.c  --  locked doubly-linked list primitives.
 * ========================================================================== */

#include "Vmm.h"


VOID
InitializeListHead (
    PLIST_ENTRY ListHead
)
{
    ListHead->Flink = ListHead->Blink = ListHead;
    return;
}

BOOLEAN
IsListEmpty (
    PLIST_ENTRY ListHead
)
{
    return (BOOLEAN) (ListHead->Flink == ListHead);
}

VOID
InsertTailList (
    PLIST_ENTRY ListHead,
    PLIST_ENTRY Entry
)
{
    PLIST_ENTRY Blink;
    Blink = ListHead->Blink;
    Entry->Flink = ListHead;
    Entry->Blink = Blink;
    Blink->Flink = Entry;
    ListHead->Blink = Entry;
    return;
}

PLIST_ENTRY
RemoveHeadList (
    PLIST_ENTRY ListHead
)
{
    PLIST_ENTRY Flink;
    PLIST_ENTRY Entry;

    Entry = ListHead->Flink;
    Flink = Entry->Flink;
    ListHead->Flink = Flink;
    Flink->Blink = ListHead;

    return Entry;
}

BOOLEAN
RemoveEntryList (
    PLIST_ENTRY Entry
)
{
    PLIST_ENTRY Blink;
    PLIST_ENTRY Flink;

    Flink = Entry->Flink;
    Blink = Entry->Blink;
    Blink->Flink = Flink;
    Flink->Blink = Blink;

    return (BOOLEAN) (Flink == Blink);
}

VOID
InitializeLockedList(PLOCKED_LIST list) {
    InitializeListHead(&list->head);
    list->count = 0;
    InitializeCriticalSectionAndSpinCount(&list->lock, 4000);
}

VOID
LockedInsertTail(PLOCKED_LIST list, PLIST_ENTRY entry) {
    EnterCriticalSection(&list->lock);
    InsertTailList(&list->head, entry);
    list->count++;
    LeaveCriticalSection(&list->lock);
}

VOID
LockedInsertTailBatch(PLOCKED_LIST list, PLIST_ENTRY* entries, int n) {
    if (n <= 0) return;
    EnterCriticalSection(&list->lock);
    for (int i = 0; i < n; i++) {
        InsertTailList(&list->head, entries[i]);
    }
    list->count += n;
    LeaveCriticalSection(&list->lock);
}

PLIST_ENTRY
LockedRemoveHead(PLOCKED_LIST list) {
    PLIST_ENTRY entry = NULL;
    EnterCriticalSection(&list->lock);
    if (!IsListEmpty(&list->head)) {
        entry = RemoveHeadList(&list->head);
        list->count--;
        entry->Flink = NULL;
        entry->Blink = NULL;
    }
    LeaveCriticalSection(&list->lock);
    return entry;
}

/* Returns TRUE if we unlinked it; FALSE if it was already off the list.
 * NULL links are our "not on any list" sentinel -- LockedRemoveHead and this
 * function both null them on removal, so a double-remove is detectable rather
 * than corrupting. */
BOOL
LockedTryRemoveEntry(PLOCKED_LIST list, PLIST_ENTRY entry) {
    BOOL removed = FALSE;
    EnterCriticalSection(&list->lock);
    if (entry->Flink != NULL) {
        RemoveEntryList(entry);
        list->count--;
        entry->Flink = NULL;
        entry->Blink = NULL;
        removed = TRUE;
    }
    LeaveCriticalSection(&list->lock);
    return removed;
}
