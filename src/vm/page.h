#ifndef PAGE_H
#define PAGE_H
#include <hash.h>

typedef enum {VM_STACK, VM_SEGMENT} vm_type;
struct SPT_elem
{
  void *vaddr;
  vm_type type;
  void *aux;
  void *paddr;
  struct hash_elem elem;
};

unsigned page_hash_func(const struct hash_elem *, void *);
bool page_less_func(const struct hash_elem *, const struct hash_elem *, void *);

bool palloc_user_page(void *, vm_type, void *);
bool palloc_free_user_page(void *);
bool vm_install_page(struct SPT_elem *, bool);

#endif /* vm/page.h */
