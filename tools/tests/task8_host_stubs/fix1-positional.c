/* SPDX-License-Identifier: Apache-2.0 */
/* Unsupported branches deliberately trap: this fixture exercises only the
 * actual positional regular/block dispatcher and production I/O engine. */
#include "../../../kernel/syscall/internal.h"
#include <stdlib.h>

int64_t syscall_io_read(uint64_t a,uint64_t b,uint64_t c)
{ (void)a;(void)b;(void)c;abort(); }
int64_t syscall_io_write(uint64_t a,uint64_t b,uint64_t c)
{ (void)a;(void)b;(void)c;abort(); }
int64_t syscall_sendfile(uint64_t a,uint64_t b,uint64_t c,uint64_t d)
{ (void)a;(void)b;(void)c;(void)d;abort(); }
int64_t syscall_copy_file_range(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;abort(); }
int64_t syscall_vector_io(struct task *t,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g)
{ (void)t;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;abort(); }
int64_t syscall_getdents_object(struct task *t,uint64_t a,int32_t b,uint64_t c,uint32_t d)
{ (void)t;(void)a;(void)b;(void)c;(void)d;abort(); }
int64_t syscall_seek_object(struct task *t,int32_t a,int64_t b,uint32_t c)
{ (void)t;(void)a;(void)b;(void)c;abort(); }
int64_t character_file_seek(struct character_file *h,struct task_file *f,int64_t o,unsigned w)
{ (void)h;(void)f;(void)o;(void)w;abort(); }
int task_device_is(const struct task_file *f,uint32_t kind)
{ (void)f;(void)kind;abort(); }
