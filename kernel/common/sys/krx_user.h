/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file user.h
 * @brief File descriptor table & management.
 */

#ifndef ECX_SYS_USER_H
#define ECX_SYS_USER_H

#include <sys/user.h>

struct file;
struct proc;
typedef struct uf_info uf_info_t;

uf_info_t *uf_new(void);
uf_info_t *uf_fork(uf_info_t *);
void uf_destroy(uf_info_t *info);

struct file *uf_lookup(uf_info_t *info, int fd);
int uf_reserve_fd(uf_info_t *info, unsigned int start_fd, unsigned int flags);
void uf_unreserve_fd(uf_info_t *info, int fd);
void uf_install_reserved(uf_info_t *info, int fd, struct file *file);

int sys_dup(int oldfd);
int sys_dup2(int oldfd, int newfd);
int sys_dup3(int oldfd, int newfd, unsigned int flags);
int sys_fcntl(int fd, int cmd, unsigned long arg);
int sys_close(int fd);

#endif /* ECX_SYS_USER_H */
