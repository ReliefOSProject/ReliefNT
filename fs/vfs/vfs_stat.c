#include "internal.h"



int storage_stat_path(const char *path, struct reliefos_stat *st)
{
    struct storage_node node;
    if (!st) {
        return -22;
    }
    int ret = storage_lookup_path(path, &node);
    if (ret < 0) {
        return ret;
    }
    st->type = node.type;
    st->reserved = 0;
    st->size = node.size;
    return 0;
}
