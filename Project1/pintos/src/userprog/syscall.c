#include "userprog/syscall.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "lib/kernel/stdio.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"

static void syscall_handler (struct intr_frame *);
static void exit_with_status (int status) NO_RETURN;
static void validate_user_buffer (const void *buffer, size_t size,
                                  bool writable);
static uint32_t read_user_argument (const struct intr_frame *f,
                                    unsigned index);
static char *copy_user_string (const char *source);

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
validate_user_buffer (const void *buffer, size_t size, bool writable)
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
    {
      const void *page_address = (const void *) address;
      uint32_t *pd = thread_current ()->pagedir;
      if (pagedir_get_page (pd, page_address) == NULL
          || (writable && !pagedir_is_writable (pd, page_address)))
        exit_with_status (-1);
    }
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
  validate_user_buffer (source, sizeof value, false);
  memcpy (&value, source, sizeof value);
  return value;
}

/* Copy a null-terminated command line into a kernel-owned page. */
static char *
copy_user_string (const char *source)
{
  char *copy = palloc_get_page (0);
  uintptr_t first = (uintptr_t) source;
  size_t i;

  if (copy == NULL)
    return NULL;

  for (i = 0; i < PGSIZE; i++)
    {
      uintptr_t address = first + i;
      if (address < first || address >= (uintptr_t) PHYS_BASE
          || pagedir_get_page (thread_current ()->pagedir,
                               (const void *) address) == NULL)
        {
          palloc_free_page (copy);
          exit_with_status (-1);
        }
      copy[i] = *(const char *) address;
      if (copy[i] == '\0')
        return copy;
    }

  palloc_free_page (copy);
  return NULL;
}

static void
syscall_handler (struct intr_frame *f)
{
  uint32_t call = read_user_argument (f, 0);

  switch (call)
    {
    case SYS_HALT:
      shutdown_power_off ();
      break;

    case SYS_EXIT:
      exit_with_status ((int) read_user_argument (f, 1));
      break;

    case SYS_EXEC:
      {
        const char *source = (const char *) read_user_argument (f, 1);
        char *cmdline = copy_user_string (source);
        if (cmdline == NULL)
          f->eax = -1;
        else
          {
            f->eax = process_execute (cmdline);
            palloc_free_page (cmdline);
          }
      }
      break;

    case SYS_WAIT:
      f->eax = process_wait ((tid_t) read_user_argument (f, 1));
      break;

    case SYS_READ:
      {
        int fd = (int) read_user_argument (f, 1);
        uint8_t *buffer = (uint8_t *) read_user_argument (f, 2);
        unsigned size = read_user_argument (f, 3);
        unsigned i;

        validate_user_buffer (buffer, size, true);
        if (fd == 0)
          {
            for (i = 0; i < size; i++)
              buffer[i] = input_getc ();
            f->eax = size;
          }
        else
          f->eax = -1;
      }
      break;

    case SYS_WRITE:
      {
        int fd = (int) read_user_argument (f, 1);
        const void *buffer = (const void *) read_user_argument (f, 2);
        unsigned size = read_user_argument (f, 3);

        validate_user_buffer (buffer, size, false);
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
