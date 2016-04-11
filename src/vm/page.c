#include "vm/page.h"
#include <stdbool.h>
#include <hash.h>
#include <debug.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
unsigned page_hash_func(const struct hash_elem *e, void *aux)
{
    ASSERT(aux == NULL);
    struct SPT_elem *SPT_elem = hash_entry(e, struct SPT_elem, elem);
    return (unsigned)SPT_elem->vaddr;
}

bool page_less_func(const struct hash_elem *a, const struct hash_elem *b, void *aux)
{
    ASSERT(aux == NULL);
    return hash_entry(a, struct SPT_elem, elem)->vaddr > hash_entry(b, struct SPT_elem, elem)->vaddr;
}


bool palloc_user_page(void *upage, vm_type type, void *aux)
{
    enum intr_level old_level = intr_disable();
    struct thread *t = thread_current();
    bool success = false;
    struct SPT_elem *SPT_elem = (struct SPTE_elem *)malloc(sizeof(struct SPT_elem));
    if(SPT_elem != NULL)
    {
      success = true;
      SPT_elem->type = type;
      SPT_elem->vaddr = upage;
      SPT_elem->aux = aux;
      SPT_elem->paddr = NULL;
      hash_insert(&t->SPT, &SPT_elem->elem);
    }
    intr_set_level(old_level);
    return success;
}

bool
vm_install_page (struct SPT_elem *SPT_elem, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, SPT_elem->vaddr) == NULL
          && pagedir_set_page (t->pagedir, SPT_elem->vaddr, SPT_elem->paddr, writable));
}
