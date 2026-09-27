#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "lib/kernel/stdio.h"
#include "threads/interrupt.h"
#include "threads/thread.h"

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f)
{
  uint32_t *args = f->esp;

  switch (args[0])
    {
    case SYS_EXIT:
      thread_current ()->exit_status = (int) args[1];
      printf ("%s: exit(%d)\n", thread_name (),
              thread_current ()->exit_status);
      thread_exit ();
      break;

    case SYS_WRITE:
      if ((int) args[1] == 1)
        {
          putbuf ((const char *) args[2], args[3]);
          f->eax = args[3];
        }
      else
        f->eax = -1;
      break;

    default:
      thread_exit ();
      break;
    }
}
