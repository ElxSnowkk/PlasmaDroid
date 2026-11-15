/*
 * Copyright (C) 2016 Vincent Hage
 */

#include <arpa/inet.h>

#include "cli/note.h"
#include "extension/portmap/portmap.h"

void initialize_portmap(PortMap *portmap)
{
    int i;

    for (i = 0; i < PORTMAP_SIZE; i++) {
	portmap->map[i].port_in = PORTMAP_DEFAULT_VALUE;
	portmap->map[i].port_out = PORTMAP_DEFAULT_VALUE;
    }

    portmap->table_mask = PORTMAP_SIZE - 1;
}

uint16_t get_index(PortMap *portmap, uint16_t key)
{
    int i = 0;
    uint16_t index;

    index = key & portmap->table_mask;

    while (index < PORTMAP_SIZE &&
	   portmap->map[index].port_in != PORTMAP_DEFAULT_VALUE &&
	   portmap->map[index].port_in != key) {
	index++;
	i++;
    }

    if (index < PORTMAP_SIZE)
	return index;

    index = 0;

    while (i < PORTMAP_SIZE &&
	   portmap->map[index].port_in != PORTMAP_DEFAULT_VALUE &&
	   portmap->map[index].port_in != key) {
	index++;
	i++;
    }

    if (i < PORTMAP_SIZE)
	return index;
    else
	return PORTMAP_SIZE;
}

int add_entry(PortMap *portmap, uint16_t port_in, uint16_t port_out)
{
    Tracee *tracee = TRACEE(global_portmap_extension);
    uint16_t index = get_index(portmap, port_in);

    if (index == PORTMAP_SIZE)
	return -1;

    portmap->map[index].port_in = port_in;
    portmap->map[index].port_out = port_out;

    VERBOSE(tracee, PORTMAP_VERBOSITY,
	    "new port mapping entry: %d -> %d", ntohs(port_in),
	    ntohs(port_out));

    return 0;
}

uint16_t get_port(PortMap *portmap, uint16_t port_in)
{
    uint16_t index = get_index(portmap, port_in);

    if (index == PORTMAP_SIZE)
	return PORTMAP_DEFAULT_VALUE;

    if (portmap->map[index].port_in == port_in)
	return portmap->map[index].port_out;
    else
	return PORTMAP_DEFAULT_VALUE;
}
