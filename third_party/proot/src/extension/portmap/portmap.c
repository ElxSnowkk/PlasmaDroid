/*
 * Copyright (C) 2016 Vincent Hage
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <linux/net.h>
#include "cli/note.h"
#include "extension/extension.h"
#include "tracee/mem.h"
#include "syscall/chain.h"
#include "extension/portmap/portmap.h"

Extension *global_portmap_extension = NULL;

typedef struct Config {
    PortMap portmap;
    bool netcoop_mode;
    bool need_to_check_new_port;
    uint16_t old_port;
    word_t sockfd;
} Config;

int change_inet_socket_port(Tracee *tracee, Config *config, word_t sockfd,
			    struct sockaddr_in *sockaddr, bool bind_mode)
{
    uint16_t port_in, port_out;

    port_in = sockaddr->sin_port;
    port_out = get_port(&config->portmap, port_in);

    if (port_out == PORTMAP_DEFAULT_VALUE) {
	if (bind_mode && config->netcoop_mode
	    && !config->need_to_check_new_port) {
	    VERBOSE(tracee, PORTMAP_VERBOSITY,
		    "ipv4 netcoop mode with: %d", htons(port_in));
	    sockaddr->sin_port = 0;
	    config->old_port = port_in;
	    config->need_to_check_new_port = true;
	    config->sockfd = sockfd;
	    return 1;
	}

	VERBOSE(tracee, PORTMAP_VERBOSITY,
		"ipv4 port ignored: %d ", htons(port_in));
	return 0;
    }

    sockaddr->sin_port = port_out;
    VERBOSE(tracee, PORTMAP_VERBOSITY,
	    "ipv4 port translation: %d -> %d (NOT GUARANTEED: bind might still fail on target port)",
	    htons(port_in), htons(port_out));

    return 1;
}

int change_inet6_socket_port(Tracee *tracee, Config *config, word_t sockfd,
			     struct sockaddr_in6 *sockaddr, bool bind_mode)
{
    uint16_t port_in, port_out;

    port_in = sockaddr->sin6_port;
    port_out = get_port(&config->portmap, port_in);

    if (port_out == PORTMAP_DEFAULT_VALUE) {
	if (bind_mode && config->netcoop_mode
	    && !config->need_to_check_new_port) {
	    VERBOSE(tracee, PORTMAP_VERBOSITY,
		    "ipv6 netcoop mode with: %d", htons(port_in));
	    sockaddr->sin6_port = 0;
	    config->old_port = port_in;
	    config->need_to_check_new_port = true;
	    config->sockfd = sockfd;
	    return 1;
	}

	VERBOSE(tracee, PORTMAP_VERBOSITY,
		"ipv6 port ignored: %d ", htons(port_in));
	return 0;
    }

    sockaddr->sin6_port = port_out;
    VERBOSE(tracee, PORTMAP_VERBOSITY,
	    "ipv6 port translation: %d -> %d (NOT GUARANTEED: bind might still fail on target port)",
	    htons(port_in), htons(port_out));

    return 1;
}

int prepare_getsockname_chained_syscall(Tracee *tracee, Config *config,
					word_t sockfd, int is_socketcall)
{
    int status = 0;
    word_t sock_addr, size_addr;
    struct sockaddr_un sockaddr;
    socklen_t size;

    size = sizeof(sockaddr);

    if (sockfd != config->sockfd)
	return 0;

    sock_addr = alloc_mem(tracee, sizeof(sockaddr));
    if (sock_addr == 0)
	return -EFAULT;
    size_addr = alloc_mem(tracee, sizeof(socklen_t));
    if (size_addr == 0)
	return -EFAULT;

    memset(&sockaddr, '\0', sizeof(sockaddr));

    status = write_data(tracee, sock_addr, &sockaddr, sizeof(sockaddr));
    if (status < 0)
	return status;
    status = write_data(tracee, size_addr, &size, sizeof(size));
    if (status < 0)
	return status;

    if (!is_socketcall) {
	status = register_chained_syscall(tracee, PR_getsockname, sockfd,
					  sock_addr, size_addr, 0, 0, 0);
    } else {
	unsigned long args[6];

	args[0] = sockfd;
	args[1] = sock_addr;
	args[2] = size_addr;
	args[3] = 0;
	args[4] = 0;
	args[5] = 0;

	word_t args_addr = alloc_mem(tracee, 6 * sizeof_word(tracee));

	status =
	    write_data(tracee, args_addr, &args, 6 * sizeof_word(tracee));
	if (status < 0)
	    return status;

	status = register_chained_syscall(tracee, PR_socketcall, SYS_GETSOCKNAME, args_addr,
					  0, 0, 0, 0);
    }

    if (status < 0)
	return status;

    return 0;
}

int translate_port(Tracee *tracee, Config *config, word_t sockfd,
		   word_t *sock_addr, int size, int is_bind_syscall)
{
    struct sockaddr_un sockaddr;
    int status;

    if (sock_addr == 0)
	return 0;

    memset(&sockaddr, '\0', sizeof(sockaddr));

    status = read_data(tracee, &sockaddr, *sock_addr, size);

    if (status < 0)
	return status;

    status = 0;
    if (sockaddr.sun_family == AF_INET) {
	status =
	    change_inet_socket_port(tracee, config, sockfd,
				    (struct sockaddr_in *)
				    &sockaddr, is_bind_syscall);
    } else if (sockaddr.sun_family == AF_INET6) {
	status =
	    change_inet6_socket_port(tracee, config, sockfd,
				     (struct sockaddr_in6 *)
				     &sockaddr, is_bind_syscall);
    }

    if (status <= 0) {
	return status;
    }

    *sock_addr = alloc_mem(tracee, sizeof(sockaddr));
    if (*sock_addr == 0)
	return -EFAULT;

    status = write_data(tracee, *sock_addr, &sockaddr, sizeof(sockaddr));
    if (status < 0)
	return status;

    return 0;
}

static int handle_sysenter_end(Tracee *tracee, Config *config)
{
    int status;
    int sysnum;

    sysnum = get_sysnum(tracee, CURRENT);

    switch (sysnum) {
#define SYSARG_ADDR(n) (args_addr + ((n) - 1) * sizeof_word(tracee))

#define PEEK_WORD(addr, forced_errno)		\
	peek_word(tracee, addr);		\
	if (errno != 0) {			\
		status = forced_errno ?: -errno; \
		break;				\
	}

#define POKE_WORD(addr, value)			\
	poke_word(tracee, addr, value);		\
	if (errno != 0) {			\
		status = -errno;		\
		break;				\
	}

    case PR_socketcall:{
	    word_t sockfd;
	    word_t args_addr;
	    word_t sock_addr_saved;
	    word_t sock_addr;
	    word_t size;
	    word_t call;
	    int is_bind_syscall = sysnum == PR_bind;

	    call = peek_reg(tracee, CURRENT, SYSARG_1);
	    is_bind_syscall = call == SYS_BIND;
	    args_addr = peek_reg(tracee, CURRENT, SYSARG_2);

	    switch (call) {
	    case SYS_BIND:
	    case SYS_CONNECT:{
		    sockfd = PEEK_WORD(SYSARG_ADDR(1), 0);
		    sock_addr = PEEK_WORD(SYSARG_ADDR(2), 0);
		    size = PEEK_WORD(SYSARG_ADDR(3), 0);

		    sock_addr_saved = sock_addr;
		    status =
			translate_port(tracee, config,
				       sockfd,
				       &sock_addr, size, is_bind_syscall);
		    if (status < 0)
			break;

		    poke_reg(tracee, SYSARG_5, sock_addr_saved);
		    poke_reg(tracee, SYSARG_6, size);

		    POKE_WORD(SYSARG_ADDR(2), sock_addr);
		    POKE_WORD(SYSARG_ADDR(3), sizeof(struct sockaddr_un));
		    return 0;
		}
	    case SYS_LISTEN:{
		    word_t sockfd;

		    if (!config->netcoop_mode
			|| !config->need_to_check_new_port)
			return 0;

		    sockfd = PEEK_WORD(SYSARG_ADDR(1), 0);

		    status =
			prepare_getsockname_chained_syscall
			(tracee, config, sockfd, true);

		    return status;
		}
	    default:
		return 0;
	    }
	    break;
	}

#undef SYSARG_ADDR
#undef PEEK_WORD
#undef POKE_WORD

    case PR_connect:
    case PR_bind:{
	    int size;
	    int is_bind_syscall;
	    word_t sockfd, sock_addr;

	    sockfd = peek_reg(tracee, CURRENT, SYSARG_1);
	    sock_addr = peek_reg(tracee, CURRENT, SYSARG_2);
	    size = (int) peek_reg(tracee, CURRENT, SYSARG_3);
	    is_bind_syscall = sysnum == PR_bind;

	    status =
		translate_port(tracee, config, sockfd,
			       &sock_addr, size, is_bind_syscall);
	    if (status < 0) {
		return status;
	    }

	    poke_reg(tracee, SYSARG_2, sock_addr);
	    poke_reg(tracee, SYSARG_3, sizeof(struct sockaddr_un));

	    return 0;
	}
    case PR_listen:{
	    word_t sockfd;

	    if (!config->netcoop_mode || !config->need_to_check_new_port)
		return 0;

	    sockfd = peek_reg(tracee, CURRENT, SYSARG_1);

	    status =
		prepare_getsockname_chained_syscall(tracee,
						    config, sockfd, false);
	    return status;
	}
    default:
	return 0;
    }

    return 0;
}

int add_changed_port_as_entry(Tracee *tracee, Config *config,
			      word_t sockfd, word_t sock_addr, int result)
{
    int status;
    struct sockaddr_un sockaddr;
    struct sockaddr_in *sockaddr_in;
    struct sockaddr_in6 *sockaddr_in6;
    uint16_t port_in, port_out;

    if (!config->need_to_check_new_port)
	return 0;

    if (sock_addr == 0)
	return 0;

    if (sockfd != config->sockfd)
	return 0;

    if (result < 0)
	return -result;

    memset(&sockaddr, '\0', sizeof(sockaddr));

    status = read_data(tracee, &sockaddr, sock_addr, sizeof(sockaddr));
    if (status < 0)
	return status;

    port_in = config->old_port;

    if (sockaddr.sun_family == AF_INET) {
	sockaddr_in = (struct sockaddr_in *) &sockaddr;
	port_out = sockaddr_in->sin_port;
    } else if (sockaddr.sun_family == AF_INET6) {
	sockaddr_in6 = (struct sockaddr_in6 *) &sockaddr;
	port_out = sockaddr_in6->sin6_port;
    } else
	return 0;

    add_portmap_entry(htons(port_in), htons(port_out));
    config->need_to_check_new_port = false;
    config->sockfd = 0;

    return 0;
}

static int handle_syschained_exit(Tracee *tracee, Config *config)
{
    int sysnum;

    sysnum = get_sysnum(tracee, CURRENT);

    switch (sysnum) {

#define SYSARG_ADDR(n) (args_addr + ((n) - 1) * sizeof_word(tracee))

#define PEEK_WORD(addr, forced_errno)		\
	peek_word(tracee, addr);		\
	if (errno != 0) {			\
		status = forced_errno ?: -errno; \
		break;				\
	}

#define POKE_WORD(addr, value)			\
	poke_word(tracee, addr, value);		\
	if (errno != 0) {			\
		status = -errno;		\
		break;				\
	}

    case PR_socketcall:{
	    word_t args_addr;
	    word_t call;

	    call = peek_reg(tracee, CURRENT, SYSARG_1);
	    args_addr = peek_reg(tracee, CURRENT, SYSARG_2);

	    switch (call) {
	    case SYS_GETSOCKNAME:{
		    word_t sockfd, sock_addr;
		    int result, status;

		    sockfd = PEEK_WORD(SYSARG_ADDR(1), 0);
		    sock_addr = PEEK_WORD(SYSARG_ADDR(2), 0);
		    result = peek_reg(tracee, CURRENT, SYSARG_RESULT);

		    status =
			add_changed_port_as_entry
			(tracee, config, sockfd, sock_addr, result);
		    return status;
		}
	    default:
		return 0;
	    }
	    return 0;
	}

#undef SYSARG_ADDR
#undef PEEK_WORD
#undef POKE_WORD

    case PR_getsockname:{
	    word_t sockfd, sock_addr;
	    int result;

	    sockfd = peek_reg(tracee, ORIGINAL, SYSARG_1);
	    sock_addr = peek_reg(tracee, CURRENT, SYSARG_2);
	    result = peek_reg(tracee, CURRENT, SYSARG_RESULT);

	    return add_changed_port_as_entry(tracee, config,
					     sockfd, sock_addr, result);
	}
    default:
	return 0;
    }
}

static FilteredSysnum filtered_sysnums[] = {
    { PR_bind, 0 },
    { PR_connect, 0 },
    { PR_listen, FILTER_SYSEXIT },
    { PR_socketcall, 0 },
    FILTERED_SYSNUM_END,
};

int add_portmap_entry(uint16_t port_in, uint16_t port_out)
{
    if (global_portmap_extension == NULL)
	return 0;
    else {
	Config *config =
	    talloc_get_type_abort(global_portmap_extension->config,
				  Config);
	return add_entry(&config->portmap, ntohs(port_in),
			 ntohs(port_out));
    }
}

int activate_netcoop_mode()
{
    if (global_portmap_extension != NULL) {
	Config *config =
	    talloc_get_type_abort(global_portmap_extension->config,
				  Config);
	config->netcoop_mode = true;
    }

    return 0;
}

int portmap_callback(Extension *extension, ExtensionEvent event,
		     intptr_t data1 UNUSED, intptr_t data2 UNUSED)
{
    switch (event) {
    case INITIALIZATION:{
	    Config *config;

	    if (global_portmap_extension != NULL)
		return -1;

	    extension->config = talloc_zero(extension, Config);
	    if (extension->config == NULL)
		return -1;

	    config = talloc_get_type_abort(extension->config, Config);
	    initialize_portmap(&config->portmap);
	    config->netcoop_mode = false;
	    config->need_to_check_new_port = false;
	    config->sockfd = 0;

	    extension->filtered_sysnums = filtered_sysnums;

	    global_portmap_extension = extension;
	    return 0;
	}
    case SYSCALL_ENTER_END:{
	    Tracee *tracee = TRACEE(extension);
	    Config *config = talloc_get_type_abort(extension->config,
						   Config);
	    return handle_sysenter_end(tracee, config);
	}
    case SYSCALL_CHAINED_EXIT:{
	    Tracee *tracee = TRACEE(extension);
	    Config *config = talloc_get_type_abort(extension->config,
						   Config);
	    return handle_syschained_exit(tracee, config);
	}
    case INHERIT_PARENT:{
	    return 0;
	}
    default:
	return 0;
    }
}
