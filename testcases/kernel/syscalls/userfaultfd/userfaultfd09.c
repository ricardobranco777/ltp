// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2026 SUSE LLC
 * Author: Ricardo Branco <rbranco@suse.com>
 */

/*\
 * Force a pagefault event and handle it using :manpage:`userfaultfd(2)`
 * from a different thread testing UFFDIO_SET_MODE toggling
 * UFFD_FEATURE_RWP_ASYNC at runtime.
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
	CHECK_UFFD_FEATURE(UFFD_FEATURE_RWP_ASYNC);
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

static void rwprotect_page(void)
{
	struct uffdio_rwprotect uffdio_rwprotect = {};

	uffdio_rwprotect.range.start = (unsigned long)page;
	uffdio_rwprotect.range.len = page_size;
	uffdio_rwprotect.mode = UFFDIO_RWPROTECT_MODE_RWP;

	SAFE_IOCTL(uffd, UFFDIO_RWPROTECT, &uffdio_rwprotect);
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

	rwp_fault_seen = 1;

	uffdio_rwprotect.range.start = msg.arg.pagefault.address & ~((unsigned long)page_size - 1);
	uffdio_rwprotect.range.len = page_size;

	SAFE_IOCTL(uffd, UFFDIO_RWPROTECT, &uffdio_rwprotect);

	return NULL;
}

static void run(void)
{
	pthread_t thr;
	struct uffdio_api uffdio_api = {};
	struct uffdio_register uffdio_register;
	struct uffdio_set_mode uffdio_set_mode = {};
	struct pollfd pollfd;
	unsigned char val;

	set_pages();
	rwp_fault_seen = 0;

	uffd = SAFE_USERFAULTFD(O_CLOEXEC | O_NONBLOCK, false);

	uffdio_api.api = UFFD_API;
	uffdio_api.features = UFFD_FEATURE_RWP | UFFD_FEATURE_RWP_ASYNC;

	SAFE_IOCTL(uffd, UFFDIO_API, &uffdio_api);

	uffdio_register.range.start = (unsigned long)page;
	uffdio_register.range.len = page_size;
	uffdio_register.mode = UFFDIO_REGISTER_MODE_RWP;

	SAFE_IOCTL(uffd, UFFDIO_REGISTER, &uffdio_register);

	/* Phase 1: async mode is active by default, access must auto-resolve. */
	rwprotect_page();

	val = page[0];

	if (val != PAGE_BYTE) {
		tst_res(TFAIL, "Async access corrupted content: got 0x%02x, expected 0x%02x",
			val, PAGE_BYTE);
		goto out;
	}

	pollfd.fd = uffd;
	pollfd.events = POLLIN;
	if (poll(&pollfd, 1, 0) != 0) {
		tst_res(TFAIL, "Unexpected pagefault message in async mode");
		goto out;
	}

	/* Phase 2: switch to sync mode via UFFDIO_SET_MODE. */
	uffdio_set_mode.disable = UFFD_FEATURE_RWP_ASYNC;

	SAFE_IOCTL(uffd, UFFDIO_SET_MODE, &uffdio_set_mode);

	rwprotect_page();

	SAFE_PTHREAD_CREATE(&thr, NULL, (void *)handle_thread, NULL);

	val = page[0];

	SAFE_PTHREAD_JOIN(thr, NULL);

	if (!rwp_fault_seen) {
		tst_res(TFAIL, "No RWPROTECT pagefault observed after switching to sync mode");
		goto out;
	}

	if (val != PAGE_BYTE) {
		tst_res(TFAIL, "Sync access corrupted content: got 0x%02x, expected 0x%02x",
			val, PAGE_BYTE);
		goto out;
	}

	tst_res(TPASS, "UFFDIO_SET_MODE toggled RWP_ASYNC correctly!");

out:
	reset_pages();
}

static struct tst_test test = {
	.setup = setup,
	.test_all = run,
	.min_kver = "7.2",
	.cleanup = reset_pages,
};
