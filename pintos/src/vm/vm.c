#include "vm/vm.h"
#include <bitmap.h>
#include <debug.h>
#include <hash.h>
#include <list.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "devices/block.h"
#include "filesys/file.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"

#define STACK_LIMIT (8 * 1024 * 1024)
#define PAGE_SECTORS (PGSIZE / BLOCK_SECTOR_SIZE)

struct vm_mapping
  {
    int id;
    struct file *file;
    uint8_t *base;
    size_t pages;
    struct list_elem elem;
  };

struct vm_page
  {
    void *upage;
    void *kpage;
    struct thread *owner;
    bool writable;
    unsigned pins;
    size_t swap_slot;
    bool private_data;
    struct file *file;
    struct vm_mapping *mapping;
    off_t offset;
    size_t read_bytes;
    struct hash_elem hash_elem;
    struct list_elem frame_elem;
  };

struct vm_space
  {
    struct hash pages;
    struct list mappings;
    int next_mapping;
  };

/* All frame/SPT changes precede file-system locking, never the reverse. */
/* shortcut: VM I/O is serialized; split locks if concurrent faults bottleneck. */
static struct lock vm_lock;
static struct list frames;
static struct list_elem *clock_hand;
static struct condition frame_available;
static struct block *swap_device;
static struct bitmap *swap_slots;

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
  clock_hand = list_end (&frames);
  cond_init (&frame_available);
  swap_device = block_get_role (BLOCK_SWAP);
  if (swap_device != NULL)
    {
      swap_slots = bitmap_create (block_size (swap_device) / PAGE_SECTORS);
      if (swap_slots == NULL)
        PANIC ("Cannot allocate swap bitmap");
    }
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
  list_init (&space->mappings);
  space->next_mapping = 0;
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
  p->swap_slot = BITMAP_ERROR;
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

static void
remove_frame (struct vm_page *p)
{
  if (clock_hand == &p->frame_elem)
    clock_hand = list_next (clock_hand);
  list_remove (&p->frame_elem);
}

static void
swap_io (size_t slot, void *kpage, bool write)
{
  size_t i;
  ASSERT (swap_device != NULL && slot < bitmap_size (swap_slots));
  ASSERT (bitmap_test (swap_slots, slot));
  for (i = 0; i < PAGE_SECTORS; i++)
    if (write)
      block_write (swap_device, slot * PAGE_SECTORS + i,
                   (uint8_t *) kpage + i * BLOCK_SECTOR_SIZE);
    else
      block_read (swap_device, slot * PAGE_SECTORS + i,
                  (uint8_t *) kpage + i * BLOCK_SECTOR_SIZE);
}

static void *
allocate_frame (void)
{
  for (;;)
    {
      void *kpage = palloc_get_page (PAL_USER);
      size_t remaining = 2 * list_size (&frames);
      bool unpinned = false;
      if (kpage != NULL)
        return kpage;
      while (remaining-- > 0)
        {
          struct vm_page *victim;
          uint32_t *pd;
          bool dirty;
          size_t slot = BITMAP_ERROR;
          enum intr_level old_level;
          if (clock_hand == list_end (&frames))
            clock_hand = list_begin (&frames);
          victim = list_entry (clock_hand, struct vm_page, frame_elem);
          clock_hand = list_next (clock_hand);
          if (victim->pins != 0)
            continue;
          unpinned = true;
          pd = victim->owner->pagedir;
          if (pagedir_is_accessed (pd, victim->upage)
              || pagedir_is_accessed (pd, victim->kpage))
            {
              pagedir_set_accessed (pd, victim->upage, false);
              pagedir_set_accessed (pd, victim->kpage, false);
              continue;
            }
          old_level = intr_disable ();
          dirty = pagedir_is_dirty (pd, victim->upage)
                  || pagedir_is_dirty (pd, victim->kpage);
          if (victim->mapping == NULL && (dirty || victim->private_data))
            {
              if (swap_slots != NULL)
                slot = bitmap_scan_and_flip (swap_slots, 0, 1, false);
              if (slot == BITMAP_ERROR)
                {
                  intr_set_level (old_level);
                  continue;
                }
            }
          /* Revoke access before blocking I/O; an owner fault waits on vm_lock. */
          pagedir_clear_page (pd, victim->upage);
          intr_set_level (old_level);
          kpage = victim->kpage;
          if (victim->mapping != NULL && dirty)
            {
              off_t written;
              filesys_lock_acquire ();
              written = file_write_at (victim->file, kpage,
                                       victim->read_bytes, victim->offset);
              filesys_lock_release ();
              if (written != (off_t) victim->read_bytes)
                {
                  bool restored = pagedir_set_page (pd, victim->upage,
                                                     kpage, victim->writable);
                  ASSERT (restored);
                  pagedir_set_dirty (pd, victim->upage, true);
                  continue;
                }
            }
          if (slot != BITMAP_ERROR)
            {
              swap_io (slot, kpage, true);
              victim->swap_slot = slot;
              victim->private_data = true;
            }
          remove_frame (victim);
          victim->kpage = NULL;
          return kpage;
        }
      if (unpinned || list_empty (&frames))
        return NULL;
      cond_wait (&frame_available, &vm_lock);
    }
}

static bool
claim (struct vm_page *p)
{
  void *kpage;
  bool ok = true;
  ASSERT (lock_held_by_current_thread (&vm_lock));
  if (p->kpage != NULL)
    return true;
  kpage = allocate_frame ();
  if (kpage == NULL)
    return false;
  if (p->swap_slot != BITMAP_ERROR)
    swap_io (p->swap_slot, kpage, false);
  else
    {
      memset (kpage, 0, PGSIZE);
      if (p->read_bytes != 0)
        {
          filesys_lock_acquire ();
          ok = file_read_at (p->file, kpage, p->read_bytes, p->offset)
               == (off_t) p->read_bytes;
          filesys_lock_release ();
        }
    }
  if (!ok || !pagedir_set_page (p->owner->pagedir, p->upage,
                                kpage, p->writable))
    {
      palloc_free_page (kpage);
      return false;
    }
  if (p->swap_slot != BITMAP_ERROR)
    {
      bitmap_reset (swap_slots, p->swap_slot);
      p->swap_slot = BITMAP_ERROR;
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
  cond_signal (&frame_available, &vm_lock);
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
      remove_frame (p);
      palloc_free_page (p->kpage);
    }
  if (p->swap_slot != BITMAP_ERROR)
    bitmap_reset (swap_slots, p->swap_slot);
  free (p);
}

/* Failed writeback keeps a live mapping intact for a later retry. */
static bool
remove_mapping (struct vm_mapping *mapping, bool writeback)
{
  size_t i;
  if (writeback)
    for (i = 0; i < mapping->pages; i++)
      {
        struct vm_page *p = lookup (mapping->base + i * PGSIZE);
        if (p->kpage != NULL
            && (pagedir_is_dirty (p->owner->pagedir, p->upage)
                || pagedir_is_dirty (p->owner->pagedir, p->kpage)))
          {
            off_t written;
            filesys_lock_acquire ();
            written = file_write_at (p->file, p->kpage,
                                     p->read_bytes, p->offset);
            filesys_lock_release ();
            if (written != (off_t) p->read_bytes)
              return false;
            pagedir_set_dirty (p->owner->pagedir, p->upage, false);
            pagedir_set_dirty (p->owner->pagedir, p->kpage, false);
          }
      }
  for (i = 0; i < mapping->pages; i++)
    {
      struct vm_page *p = lookup (mapping->base + i * PGSIZE);
      ASSERT (p != NULL && p->mapping == mapping);
      hash_delete (&thread_current ()->vm->pages, &p->hash_elem);
      free_page (&p->hash_elem, NULL);
    }
  list_remove (&mapping->elem);
  filesys_lock_acquire ();
  file_close (mapping->file);
  filesys_lock_release ();
  free (mapping);
  cond_broadcast (&frame_available, &vm_lock);
  return true;
}

int
vm_mmap (struct file *file, void *address)
{
  struct vm_space *space = thread_current ()->vm;
  struct vm_mapping *mapping;
  struct file *reopened;
  off_t length, offset;
  size_t i, pages;
  int id = -1;
  if (space == NULL || file == NULL || address == NULL || pg_ofs (address) != 0
      || !is_user_vaddr (address))
    return -1;
  lock_acquire (&vm_lock);
  filesys_lock_acquire ();
  length = file_length (file);
  reopened = file_reopen (file);
  filesys_lock_release ();
  if (reopened == NULL)
    goto done;
  if (length <= 0 || (size_t) length > (uintptr_t) PHYS_BASE - (uintptr_t) address
      || space->next_mapping == INT_MAX)
    goto close_file;
  pages = ((size_t) length + PGSIZE - 1) / PGSIZE;
  for (i = 0; i < pages; i++)
    if (lookup ((uint8_t *) address + i * PGSIZE) != NULL)
      goto close_file;
  mapping = calloc (1, sizeof *mapping);
  if (mapping == NULL)
    goto close_file;
  mapping->id = space->next_mapping;
  mapping->file = reopened;
  mapping->base = address;
  list_push_back (&space->mappings, &mapping->elem);
  for (offset = 0; offset < length; )
    {
      size_t bytes = (size_t) (length - offset);
      struct vm_page *p;
      if (bytes > PGSIZE)
        bytes = PGSIZE;
      p = add_page ((uint8_t *) address + (size_t) offset,
                    reopened, offset, bytes, true);
      if (p == NULL)
        {
          remove_mapping (mapping, false);
          goto done;
        }
      p->mapping = mapping;
      mapping->pages++;
      offset += bytes;
    }
  id = space->next_mapping++;
  goto done;

close_file:
  filesys_lock_acquire ();
  file_close (reopened);
  filesys_lock_release ();
done:
  lock_release (&vm_lock);
  return id;
}

bool
vm_munmap (int id)
{
  struct vm_space *space = thread_current ()->vm;
  struct list_elem *e;
  bool ok = true;
  if (space == NULL)
    return true;
  lock_acquire (&vm_lock);
  for (e = list_begin (&space->mappings); e != list_end (&space->mappings);
       e = list_next (e))
    {
      struct vm_mapping *mapping = list_entry (e, struct vm_mapping, elem);
      if (mapping->id == id)
        {
          ok = remove_mapping (mapping, true);
          break;
        }
    }
  lock_release (&vm_lock);
  return ok;
}

void
vm_destroy (void)
{
  struct thread *t = thread_current ();
  if (t->vm == NULL)
    return;
  lock_acquire (&vm_lock);
  while (!list_empty (&t->vm->mappings))
    {
      struct vm_mapping *mapping = list_entry (
        list_front (&t->vm->mappings), struct vm_mapping, elem);
      if (!remove_mapping (mapping, true))
        {
          printf ("%s: mmap writeback failed during exit\n", t->name);
          remove_mapping (mapping, false);
        }
    }
  hash_destroy (&t->vm->pages, free_page);
  free (t->vm);
  t->vm = NULL;
  cond_broadcast (&frame_available, &vm_lock);
  lock_release (&vm_lock);
}
