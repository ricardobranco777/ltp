// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2026 SUSE LLC
 * Author: Ricardo Branco <rbranco@suse.com>
 */

/*\
 * Force a pagefault event and handle it using :manpage:`userfaultfd(2)`
 * from a different thread testing UFFDIO_RWPROTECT.
 *
 * - Sync: a read of an RWP-protected page blocks and is delivered as a fault.
 * - Async: with UFFD_FEATURE_RWP_ASYNC the access resolves automatically,
 *   then UFFDIO_SET_MODE disables async mode and the access is delivered
 *   as a fault again.
 */

#include "config.h"
#include <poll.h>
#include "tst_test.h"
#include "tst_safe_macros.h"
#include "tst_safe_pthread.h"
#include "lapi/userfaultfd.h"

#define PAGE_BYTE 0x5a

static long page_size;
static char *page;
static int uffd = -1;
static volatile int rwp_fault_seen;

static void setup(void)
{
	CHECK_UFFD_FEATURE(UFFD_FEATURE_RWP);
	page_size = SAFE_SYSCONF(_SC_PAGE_SIZE);
}

static void set_page(void)
{
	page = SAFE_MMAP(NULL, page_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	memset(page, PAGE_BYTE, page_size);
}

static void reset_page(void)
{
	if (page) {
		SAFE_MUNMAP(page, page_size);
		page = NULL;
	}
	if (uffd != -1)
		SAFE_CLOSE(uffd);
}

static void rwprotect(unsigned long start, uint64_t mode)
{
	struct uffdio_rwprotect uffdio_rwprotect = {
		.range.start = start,
		.range.len = page_size,
		.mode = mode,
	};

	SAFE_IOCTL(uffd, UFFDIO_RWPROTECT, &uffdio_rwprotect);
}

static void *handle_thread(void *arg LTP_ATTRIBUTE_UNUSED)
{
	static struct uffd_msg msg;
	struct pollfd pollfd = {
		.fd = uffd,
		.events = POLLIN,
	};

	SAFE_POLL(&pollfd, 1, -1);

	SAFE_READ(SAFE_READ_ALL, uffd, &msg, sizeof(msg));

	if (msg.event != UFFD_EVENT_PAGEFAULT)
		tst_brk(TFAIL, "Received unexpected UFFD_EVENT %d", msg.event);

	if (!(msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_RWP)) {
		tst_brk(TFAIL, "Expected RWP fault, got flags=0x%llx",
			(unsigned long long)msg.arg.pagefault.flags);
	}

	/* The access is a read, so WRITE must not be set. */
	if (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE)
		tst_brk(TFAIL, "Unexpected WRITE flag on a read-only access");

	rwp_fault_seen = 1;

	/* Resolve the fault by clearing RWP so the reader can resume. */
	rwprotect(msg.arg.pagefault.address & ~((unsigned long)page_size - 1), 0);

	return NULL;
}

/*
 * Protect the page and read it. The read must block and be delivered as an
 * RWP fault to the handler thread, which resolves it.
 */
static int check_sync_fault(void)
{
	pthread_t thr;
	unsigned char val;

	rwp_fault_seen = 0;
	rwprotect((unsigned long)page, UFFDIO_RWPROTECT_MODE_RWP);

	SAFE_PTHREAD_CREATE(&thr, NULL, handle_thread, NULL);

	/* Try to read the RWP-protected page. */
	val = page[0];

	SAFE_PTHREAD_JOIN(thr, NULL);

	if (!rwp_fault_seen) {
		tst_res(TFAIL, "No RWPROTECT pagefault observed");
		return 1;
	}

	if (val != PAGE_BYTE) {
		tst_res(TFAIL, "Page content corrupted: got 0x%02x, expected 0x%02x",
			val, PAGE_BYTE);
		return 1;
	}

	return 0;
}

/*
 * Protect the page and read it with RWP_ASYNC enabled. The read must resolve
 * automatically without queueing a fault message.
 */
static int check_async_access(void)
{
	struct pollfd pollfd = {
		.fd = uffd,
		.events = POLLIN,
	};
	unsigned char val;

	rwprotect((unsigned long)page, UFFDIO_RWPROTECT_MODE_RWP);

	val = page[0];

	if (val != PAGE_BYTE) {
		tst_res(TFAIL, "Async access corrupted content: got 0x%02x, expected 0x%02x",
			val, PAGE_BYTE);
		return 1;
	}

	if (SAFE_POLL(&pollfd, 1, 0) != 0) {
		tst_res(TFAIL, "Unexpected pagefault message in async mode");
		return 1;
	}

	return 0;
}

static void run(unsigned int n)
{
	struct uffdio_api uffdio_api = {
		.api = UFFD_API,
		.features = UFFD_FEATURE_RWP,
	};
	struct uffdio_register uffdio_register;
	struct uffdio_set_mode uffdio_set_mode = {
		.disable = UFFD_FEATURE_RWP_ASYNC,
	};

	if (n) {
		CHECK_UFFD_FEATURE(UFFD_FEATURE_RWP_ASYNC);
		uffdio_api.features |= UFFD_FEATURE_RWP_ASYNC;
	}

	set_page();

	uffd = SAFE_USERFAULTFD(O_CLOEXEC | O_NONBLOCK, false);

	SAFE_IOCTL(uffd, UFFDIO_API, &uffdio_api);

	uffdio_register.range.start = (unsigned long)page;
	uffdio_register.range.len = page_size;
	uffdio_register.mode = UFFDIO_REGISTER_MODE_RWP;

	SAFE_IOCTL(uffd, UFFDIO_REGISTER, &uffdio_register);

	if (!n) {
		if (!check_sync_fault())
			tst_res(TPASS, "RWPROTECT pagefault handled!");
		goto out;
	}

	if (check_async_access())
		goto out;

	/*
	 * Disable RWP_ASYNC at runtime. The sync check below is what verifies
	 * that UFFDIO_SET_MODE took effect.
	 */
	SAFE_IOCTL(uffd, UFFDIO_SET_MODE, &uffdio_set_mode);

	if (!check_sync_fault())
		tst_res(TPASS, "UFFDIO_SET_MODE disabled RWP_ASYNC, RWPROTECT pagefault handled!");

out:
	reset_page();
}

static struct tst_test test = {
	.setup = setup,
	.test = run,
	.tcnt = 2,
	.cleanup = reset_page,
};
