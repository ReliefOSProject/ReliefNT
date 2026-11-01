/* SPDX-License-Identifier: Apache-2.0 */
/* Registry-only model for fixtures which do not link real page-table fork. */
#include <reliefnt/paging.h>
#include <assert.h>
static struct paging_writable_alias *aliases;
void paging_writable_alias_add(struct paging_writable_alias *alias,uint64_t physical)
{ assert(alias && physical);alias->physical=physical;alias->next=aliases;aliases=alias; }
void paging_writable_alias_remove(struct paging_writable_alias *alias)
{
    struct paging_writable_alias **link=&aliases;
    while(*link && *link!=alias)link=&(*link)->next;
    assert(*link==alias);*link=alias->next;alias->next=NULL;alias->physical=0;
}
