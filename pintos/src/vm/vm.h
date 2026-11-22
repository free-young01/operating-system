#ifndef VM_VM_H
#define VM_VM_H

#include <stdbool.h>
#include <stddef.h>
#include "filesys/off_t.h"

struct file;
struct vm_space;

void vm_init (void);
bool vm_process_init (void);
void vm_destroy (void);
bool vm_add_file (void *upage, struct file *, off_t offset,
                  size_t read_bytes, bool writable);
bool vm_validate (const void *address, bool write, const void *esp);
bool vm_pin (const void *address, bool write, const void *esp);
void vm_unpin (const void *address);
int vm_mmap (struct file *, void *address);
bool vm_munmap (int id);

#endif /* vm/vm.h */
