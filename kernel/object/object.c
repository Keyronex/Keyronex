/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Feb 13 2023.
 */
/*!
 * @file object/object.c
 * @brief Implements the object manager.
 *
 * The general principles:
 * - Objects can be referred to indirectly by handles, which are integers.
 * - Executive processes store a table of handle entries, pointing to objects.
 * - Objects always store an object header as their first element.
 */

#include "kdk/object.h"
