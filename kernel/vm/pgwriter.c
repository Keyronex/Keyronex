/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Mar 11 2023.
 */

/*
 * The principle:
 *  - lock page queues
 *  - take a page from the modified queue
 *  - manually add one reference to the owner
 *  - unlock page queues
 *  - if the owner has a retention count of 1, then it is being destroyed. The
 *  page might be marked accordingly so that the owner doesn't try to take it
 *  off any list.
 *  - otherwise the owner still lives; lock the owner, verify the page still
 *  looks the same, and press ahead with the pageout.
 */