

#include "includes.h"

struct static_block_t {
    static_block_t() { proc_init(); }
};

static static_block_t static_block;
