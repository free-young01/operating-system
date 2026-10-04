#include "userprog/syscall.h"
#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "lib/kernel/stdio.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"

/* File descriptors belong to one process; the file system is shared. */
struct file_descriptor
  {
    int fd;
    struct file *file;
    struct list_elem elem;
  };

static void syscall_handler (struct intr_frame *);
static void exit_with_status (int status) NO_RETURN;
static void validate_user_buffer (const void *buffer, size_t size,
                                  bool writable);
static uint32_t read_user_argument (const struct intr_frame *f,
                                    unsigned index);
static char *copy_user_string (const char *source);
static struct file_descriptor *find_file_descriptor (int fd);

static struct lock filesys_lock;

void
syscall_init (void) 
{
  lock_init (&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

void
filesys_lock_acquire (void)
{
  lock_acquire (&filesys_lock);
}

void
filesys_lock_release (void)
{
  lock_release (&filesys_lock);
}

/* Find FD in the current process only. */
static struct file_descriptor *
find_file_descriptor (int fd)
{
  struct list *files = &thread_current ()->open_files;
  struct list_elem *e;

  for (e = list_begin (files); e != list_end (files); e = list_next (e))
    {
      struct file_descriptor *descriptor =
        list_entry (e, struct file_descriptor, elem);
      if (descriptor->fd == fd)
        return descriptor;
    }
  return NULL;
}

/* A process also closes descriptors it forgot to close explicitly. */
void
syscall_close_all (void)
{
  struct list *files = &thread_current ()->open_files;

  filesys_lock_acquire ();
  while (!list_empty (files))
    {
      struct file_descriptor *descriptor =
        list_entry (list_pop_front (files), struct file_descriptor, elem);
      file_close (descriptor->file);
      free (descriptor);
    }
  filesys_lock_release ();
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

    case SYS_CREATE:
      {
        const char *source = (const char *) read_user_argument (f, 1);
        unsigned initial_size = read_user_argument (f, 2);
        char *name = copy_user_string (source);

        f->eax = false;
        if (name != NULL && initial_size <= INT_MAX)
          {
            filesys_lock_acquire ();
            f->eax = filesys_create (name, initial_size);
            filesys_lock_release ();
          }
        if (name != NULL)
          palloc_free_page (name);
      }
      break;

    case SYS_REMOVE:
      {
        const char *source = (const char *) read_user_argument (f, 1);
        char *name = copy_user_string (source);

        f->eax = false;
        if (name != NULL)
          {
            filesys_lock_acquire ();
            f->eax = filesys_remove (name);
            filesys_lock_release ();
            palloc_free_page (name);
          }
      }
      break;

    case SYS_OPEN:
      {
        const char *source = (const char *) read_user_argument (f, 1);
        char *name = copy_user_string (source);
        struct file_descriptor *descriptor = NULL;
        struct file *file = NULL;
        struct thread *cur = thread_current ();

        f->eax = -1;
        if (name == NULL)
          break;
        descriptor = malloc (sizeof *descriptor);
        if (descriptor != NULL)
          {
            filesys_lock_acquire ();
            file = filesys_open (name);
            if (file != NULL && cur->next_fd < INT_MAX)
              {
                descriptor->fd = cur->next_fd++;
                descriptor->file = file;
                list_push_back (&cur->open_files, &descriptor->elem);
                f->eax = descriptor->fd;
              }
            else
              {
                file_close (file);
                free (descriptor);
              }
            filesys_lock_release ();
          }
        palloc_free_page (name);
      }
      break;

    case SYS_CLOSE:
      {
        int fd = (int) read_user_argument (f, 1);
        struct file_descriptor *descriptor = find_file_descriptor (fd);

        if (descriptor != NULL)
          {
            list_remove (&descriptor->elem);
            filesys_lock_acquire ();
            file_close (descriptor->file);
            filesys_lock_release ();
            free (descriptor);
          }
      }
      break;

    case SYS_FILESIZE:
      {
        int fd = (int) read_user_argument (f, 1);
        struct file_descriptor *descriptor = find_file_descriptor (fd);

        f->eax = -1;
        if (descriptor != NULL)
          {
            filesys_lock_acquire ();
            f->eax = file_length (descriptor->file);
            filesys_lock_release ();
          }
      }
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
          {
            struct file_descriptor *descriptor = find_file_descriptor (fd);
            f->eax = -1;
            if (descriptor != NULL && size <= INT_MAX)
              {
                filesys_lock_acquire ();
                f->eax = file_read (descriptor->file, buffer, size);
                filesys_lock_release ();
              }
          }
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
          {
            struct file_descriptor *descriptor = find_file_descriptor (fd);
            f->eax = -1;
            if (descriptor != NULL && size <= INT_MAX)
              {
                filesys_lock_acquire ();
                f->eax = file_write (descriptor->file, buffer, size);
                filesys_lock_release ();
              }
          }
      }
      break;

    case SYS_SEEK:
      {
        int fd = (int) read_user_argument (f, 1);
        unsigned position = read_user_argument (f, 2);
        struct file_descriptor *descriptor = find_file_descriptor (fd);

        if (descriptor != NULL && position <= INT_MAX)
          {
            filesys_lock_acquire ();
            file_seek (descriptor->file, position);
            filesys_lock_release ();
          }
      }
      break;

    case SYS_TELL:
      {
        int fd = (int) read_user_argument (f, 1);
        struct file_descriptor *descriptor = find_file_descriptor (fd);

        f->eax = -1;
        if (descriptor != NULL)
          {
            filesys_lock_acquire ();
            f->eax = file_tell (descriptor->file);
            filesys_lock_release ();
          }
      }
      break;

    case SYS_FIBONACCI:
      {
        int n = (int) read_user_argument (f, 1);
        int previous = 0;
        int current = 1;
        int next;
        int i;

        if (n < 0)
          {
            f->eax = -1;
            break;
          }
        if (n == 0)
          {
            f->eax = 0;
            break;
          }
        for (i = 2; i <= n; i++)
          {
            if (previous > INT_MAX - current)
              {
                f->eax = -1;
                break;
              }
            next = previous + current;
            previous = current;
            current = next;
          }
        if (i > n)
          f->eax = current;
      }
      break;

    case SYS_MAX_OF_FOUR_INT:
      {
        int values[4];
        int maximum;
        int i;

        for (i = 0; i < 4; i++)
          values[i] = (int) read_user_argument (f, i + 1);
        maximum = values[0];
        for (i = 1; i < 4; i++)
          if (values[i] > maximum)
            maximum = values[i];
        f->eax = maximum;
      }
      break;

    default:
      exit_with_status (-1);
      break;
    }
}
