#ifndef VM_ANON_H
#define VM_ANON_H
#include "vm/vm.h"
struct page;
enum vm_type;

struct anon_page {
    // struct page anon_p; // heesan 주의☠️ ?? 나중에 사용
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);


#endif
