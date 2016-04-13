#ifndef PAGE_H
#define PAGE_H
#include <hash.h>
#include "vm/swap.h"

typedef enum {VM_STACK, VM_SEGMENT, VM_MMAP} vm_type;
struct SPT_elem
{
  void *vaddr;
  vm_type type;
  void *aux;
  void *paddr;
  struct hash_elem elem;
  struct FRAME_elem *frame_ptr;
  struct list_elem mmap_elem;
};

struct lock page_lock;
void vm_init(void);
unsigned page_hash_func(const struct hash_elem *, void *);
bool page_less_func(const struct hash_elem *, const struct hash_elem *, void *);

bool palloc_user_page(void *, vm_type, void *);
bool palloc_free_user_page(void *);
bool vm_install(struct SPT_elem *);
bool vm_install_page(struct SPT_elem *, bool);
#endif /* vm/page.h */
