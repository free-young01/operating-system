#include "vm/vm.h"
#include <debug.h>
#include <hash.h>
#include <list.h>
#include <string.h>
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"

#define STACK_LIMIT (8 * 1024 * 1024)

struct vm_page
  {
    void *upage;
    void *kpage;
    struct thread *owner;
    bool writable;
    unsigned pins;
    struct file *file;
    off_t offset;
    size_t read_bytes;
    struct hash_elem hash_elem;
    struct list_elem frame_elem;
  };

struct vm_space
  {
    struct hash pages;
  };

/* All frame/SPT changes precede file-system locking, never the reverse. */
static struct lock vm_lock;
static struct list frames;

static unsigned
page_hash (const struct hash_elem *e, void *aux UNUSED)
{
  const struct vm_page *p = hash_entry (e, struct vm_page, hash_elem);
  return hash_bytes (&p->upage, sizeof p->upage);
}

static bool
page_less (const struct hash_elem *a, const struct hash_elem *b,
           void *aux UNUSED)
{
  return (uintptr_t) hash_entry (a, struct vm_page, hash_elem)->upage
       < (uintptr_t) hash_entry (b, struct vm_page, hash_elem)->upage;
}

static struct vm_page *
lookup (const void *address)
{
  struct vm_space *space = thread_current ()->vm;
  struct vm_page key;
  struct hash_elem *e;
  if (space == NULL)
    return NULL;
  key.upage = pg_round_down (address);
  e = hash_find (&space->pages, &key.hash_elem);
  return e != NULL ? hash_entry (e, struct vm_page, hash_elem) : NULL;
}

void
vm_init (void)
{
  lock_init (&vm_lock);
  list_init (&frames);
}

bool
vm_process_init (void)
{
  struct vm_space *space = malloc (sizeof *space);
  if (space == NULL)
    return false;
  if (!hash_init (&space->pages, page_hash, page_less, NULL))
    {
      free (space);
      return false;
    }
  thread_current ()->vm = space;
  return true;
}

static struct vm_page *
add_page (void *upage, struct file *file, off_t offset,
          size_t read_bytes, bool writable)
{
  struct vm_page *p;
  ASSERT (lock_held_by_current_thread (&vm_lock));
  ASSERT (pg_ofs (upage) == 0 && read_bytes <= PGSIZE);
  if (thread_current ()->vm == NULL || upage == NULL
      || !is_user_vaddr (upage) || lookup (upage) != NULL)
    return NULL;
  p = calloc (1, sizeof *p);
  if (p == NULL)
    return NULL;
  p->upage = upage;
  p->owner = thread_current ();
  p->file = file;
  p->offset = offset;
  p->read_bytes = read_bytes;
  p->writable = writable;
  hash_insert (&p->owner->vm->pages, &p->hash_elem);
  return p;
}

bool
vm_add_file (void *upage, struct file *file, off_t offset,
             size_t read_bytes, bool writable)
{
  bool ok;
  lock_acquire (&vm_lock);
  ok = add_page (upage, file, offset, read_bytes, writable) != NULL;
  lock_release (&vm_lock);
  return ok;
}

static struct vm_page *
validate (const void *address, bool write, const void *esp)
{
  struct vm_page *p;
  uintptr_t addr = (uintptr_t) address;
  uintptr_t sp = (uintptr_t) esp;
  if (addr < PGSIZE || addr >= (uintptr_t) PHYS_BASE)
    return NULL;
  p = lookup (address);
  if (p == NULL && sp >= PGSIZE && sp <= (uintptr_t) PHYS_BASE
      && addr >= (uintptr_t) PHYS_BASE - STACK_LIMIT && addr >= sp - 32)
    p = add_page (pg_round_down (address), NULL, 0, 0, true);
  return p != NULL && (!write || p->writable) ? p : NULL;
}

bool
vm_validate (const void *address, bool write, const void *esp)
{
  bool ok;
  lock_acquire (&vm_lock);
  ok = validate (address, write, esp) != NULL;
  lock_release (&vm_lock);
  return ok;
}

static bool
claim (struct vm_page *p)
{
  void *kpage;
  bool ok = true;
  ASSERT (lock_held_by_current_thread (&vm_lock));
  if (p->kpage != NULL)
    return true;
  kpage = palloc_get_page (PAL_USER);
  if (kpage == NULL)
    return false;
  if (p->read_bytes != 0)
    {
      filesys_lock_acquire ();
      ok = file_read_at (p->file, kpage, p->read_bytes, p->offset)
           == (off_t) p->read_bytes;
      filesys_lock_release ();
    }
  memset ((uint8_t *) kpage + p->read_bytes, 0, PGSIZE - p->read_bytes);
  if (!ok || !pagedir_set_page (p->owner->pagedir, p->upage,
                                kpage, p->writable))
    {
      palloc_free_page (kpage);
      return false;
    }
  p->kpage = kpage;
  pagedir_set_dirty (p->owner->pagedir, kpage, false);
  pagedir_set_accessed (p->owner->pagedir, kpage, false);
  list_push_back (&frames, &p->frame_elem);
  return true;
}

bool
vm_pin (const void *address, bool write, const void *esp)
{
  struct vm_page *p;
  bool ok;
  lock_acquire (&vm_lock);
  p = validate (address, write, esp);
  ok = p != NULL && claim (p);
  if (ok)
    p->pins++;
  lock_release (&vm_lock);
  return ok;
}

void
vm_unpin (const void *address)
{
  struct vm_page *p;
  lock_acquire (&vm_lock);
  p = lookup (address);
  ASSERT (p != NULL && p->kpage != NULL && p->pins > 0);
  p->pins--;
  lock_release (&vm_lock);
}

static void
free_page (struct hash_elem *e, void *aux UNUSED)
{
  struct vm_page *p = hash_entry (e, struct vm_page, hash_elem);
  ASSERT (p->pins == 0);
  if (p->kpage != NULL)
    {
      pagedir_clear_page (p->owner->pagedir, p->upage);
      list_remove (&p->frame_elem);
      palloc_free_page (p->kpage);
    }
  free (p);
}

void
vm_destroy (void)
{
  struct thread *t = thread_current ();
  if (t->vm == NULL)
    return;
  lock_acquire (&vm_lock);
  hash_destroy (&t->vm->pages, free_page);
  free (t->vm);
  t->vm = NULL;
  lock_release (&vm_lock);
}
