/* Non-receiving socket fixtures do not touch or pin user output pages. */
uint64_t address_space_user_page_phys(const struct address_space *as,uint64_t address)
{ (void)as;(void)address;abort(); }
uint64_t address_space_user_page_flags(const struct address_space *as,uint64_t address)
{ (void)as;(void)address;abort(); }
void *mm_phys_to_virt(uint64_t address) { (void)address;abort(); }
uint32_t mm_page_refcount(uint64_t address) { (void)address;abort(); }
void mm_retain_page(uint64_t address) { (void)address;abort(); }
void mm_free_page(uint64_t address) { (void)address;abort(); }
int page_cache_retain(uint64_t address) { (void)address;abort(); }
void page_cache_release(uint64_t address) { (void)address;abort(); }
void page_cache_mark_shared_writable(uint64_t address) { (void)address;abort(); }
