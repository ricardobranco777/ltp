// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2026 SUSE LLC
 * Author: Ricardo Branco <rbranco@suse.com>
 */

/*\
 * Force a pagefault event and handle it using :manpage:`userfaultfd(2)`
 * from a different thread testing UFFDIO_RWPROTECT.
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
}

static void set_pages(void)
{
	page_size = SAFE_SYSCONF(_SC_PAGE_SIZE);
	page = SAFE_MMAP(NULL, page_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	memset(page, PAGE_BYTE, page_size);
}

static void reset_pages(void)
{
	if (page) {
		SAFE_MUNMAP(page, page_size);
		page = NULL;
	}
	if (uffd != -1)
		SAFE_CLOSE(uffd);
}

static void *handle_thread(void *arg LTP_ATTRIBUTE_UNUSED)
{
	static struct uffd_msg msg;
	struct uffdio_rwprotect uffdio_rwprotect = {};

	struct pollfd pollfd;
	int nready;

	pollfd.fd = uffd;
	pollfd.events = POLLIN;
	nready = poll(&pollfd, 1, -1);
	if (nready == -1)
		tst_brk(TBROK | TERRNO, "Error on poll");

	SAFE_READ(1, uffd, &msg, sizeof(msg));

	if (msg.event != UFFD_EVENT_PAGEFAULT)
		tst_brk(TFAIL, "Received unexpected UFFD_EVENT %d", msg.event);

	if (!(msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_RWP)) {
		tst_brk(TFAIL,
			"Expected RWP fault but flags=%lx",
			(unsigned long)msg.arg.pagefault.flags);
	}

	/* The access below is a read, so WRITE must not be set. */
	if (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE)
		tst_brk(TFAIL, "Unexpected WRITE flag on a read-only access");

	rwp_fault_seen = 1;

	/* Resolve the fault by clearing RWP so the reader can resume. */
	uffdio_rwprotect.range.start = msg.arg.pagefault.address & ~((unsigned long)page_size - 1);
	uffdio_rwprotect.range.len = page_size;

	SAFE_IOCTL(uffd, UFFDIO_RWPROTECT, &uffdio_rwprotect);

	SAFE_CLOSE(uffd);
	return NULL;
}

static void run(void)
{
	pthread_t thr;
	struct uffdio_api uffdio_api = {};
	struct uffdio_register uffdio_register;
	struct uffdio_rwprotect uffdio_rwprotect;
	unsigned char val;

	set_pages();
	rwp_fault_seen = 0;

	uffd = SAFE_USERFAULTFD(O_CLOEXEC | O_NONBLOCK, false);

	uffdio_api.api = UFFD_API;
	uffdio_api.features = UFFD_FEATURE_RWP;

	SAFE_IOCTL(uffd, UFFDIO_API, &uffdio_api);

	uffdio_register.range.start = (unsigned long)page;
	uffdio_register.range.len = page_size;
	uffdio_register.mode = UFFDIO_REGISTER_MODE_RWP;

	SAFE_IOCTL(uffd, UFFDIO_REGISTER, &uffdio_register);

	uffdio_rwprotect.range.start = (unsigned long)page;
	uffdio_rwprotect.range.len = page_size;
	uffdio_rwprotect.mode = UFFDIO_RWPROTECT_MODE_RWP;

	SAFE_IOCTL(uffd, UFFDIO_RWPROTECT, &uffdio_rwprotect);

	SAFE_PTHREAD_CREATE(&thr, NULL, (void *)handle_thread, NULL);

	/* Try to read the RWP-protected page. */
	val = page[0];

	SAFE_PTHREAD_JOIN(thr, NULL);
	reset_pages();

	if (!rwp_fault_seen) {
		tst_res(TFAIL, "No RWPROTECT pagefault observed");
		return;
	}

	if (val != PAGE_BYTE) {
		tst_res(TFAIL, "Page content corrupted: got 0x%02x, expected 0x%02x",
			val, PAGE_BYTE);
		return;
	}

	tst_res(TPASS, "RWPROTECT pagefault handled!");
}

static struct tst_test test = {
	.setup = setup,
	.test_all = run,
	.min_kver = "7.2",
	.cleanup = reset_pages,
};
