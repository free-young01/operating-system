#include "userprog/syscall.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <syscall-nr.h>
#include "lib/kernel/stdio.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"

static void syscall_handler (struct intr_frame *);
static void exit_with_status (int status) NO_RETURN;
static void validate_user_buffer (const void *buffer, size_t size);
static uint32_t read_user_argument (const struct intr_frame *f,
                                    unsigned index);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* Report the exit status to the parent and stop this process. */
static void
exit_with_status (int status)
{
  thread_current ()->exit_status = status;
  printf ("%s: exit(%d)\n", thread_name (), status);
  thread_exit ();
}

/* Check every page touched by a user buffer before the kernel reads it.
   The end check also rejects ranges that wrap around or enter kernel space. */
static void
validate_user_buffer (const void *buffer, size_t size)
{
  uintptr_t first = (uintptr_t) buffer;
  uintptr_t last;
  uintptr_t address;

  if (size == 0)
    return;

  last = first + size - 1;
  if (buffer == NULL || !is_user_vaddr (buffer)
      || last < first || last >= (uintptr_t) PHYS_BASE)
    exit_with_status (-1);

  for (address = first; address <= last;
       address = (address & ~(uintptr_t) PGMASK) + PGSIZE)
    if (pagedir_get_page (thread_current ()->pagedir,
                          (const void *) address) == NULL)
      exit_with_status (-1);
}

/* Read one 32-bit syscall word, including words crossing a page boundary. */
static uint32_t
read_user_argument (const struct intr_frame *f, unsigned index)
{
  uintptr_t stack = (uintptr_t) f->esp;
  uint32_t value;
  const void *source;

  if (index > (UINTPTR_MAX - stack) / sizeof value)
    exit_with_status (-1);
  source = (const void *) (stack + index * sizeof value);
  validate_user_buffer (source, sizeof value);
  memcpy (&value, source, sizeof value);
  return value;
}

static void
syscall_handler (struct intr_frame *f)
{
  uint32_t call = read_user_argument (f, 0);

  switch (call)
    {
    case SYS_EXIT:
      exit_with_status ((int) read_user_argument (f, 1));
      break;

    case SYS_WRITE:
      {
        int fd = (int) read_user_argument (f, 1);
        const void *buffer = (const void *) read_user_argument (f, 2);
        unsigned size = read_user_argument (f, 3);

        validate_user_buffer (buffer, size);
        if (fd == 1)
          {
            putbuf (buffer, size);
            f->eax = size;
          }
        else
          f->eax = -1;
      }
      break;

    default:
      exit_with_status (-1);
      break;
    }
}
