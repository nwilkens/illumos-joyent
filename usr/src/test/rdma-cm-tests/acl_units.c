/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/*
 * The rdk_cm peer allow-list: what rdk_cm_acl_create() refuses, and that
 * rdk_cm_acl_allows() admits exactly the listed peers.
 */

#include "units.h"

static uint64_t seed = 0x2545f4914f6cdd1dULL;

static uint32_t
rnd(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return ((uint32_t)(seed >> 11));
}

static void
sin_set(struct sockaddr_in *s, uint32_t host)
{
	(void) memset(s, 0, sizeof (*s));
	s->sin_family = AF_INET;
	s->sin_addr.s_addr = htonl(host);
}

static int
create1(uint32_t host)
{
	struct sockaddr_in s;
	rdk_cm_acl_t *acl = (void *)1;
	int ret;

	sin_set(&s, host);
	ret = rdk_cm_acl_create(&s, 1, &acl);
	if (ret == 0)
		kmem_free(acl, sizeof (*acl) + sizeof (ipaddr_t));
	else
		CHECK(acl == NULL);
	return (ret);
}

static boolean_t
listed(const struct sockaddr_in *p, uint32_t n, ipaddr_t a)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (p[i].sin_addr.s_addr == a)
			return (B_TRUE);
	}
	return (B_FALSE);
}

int
main(void)
{
	static struct sockaddr_in peers[RDK_CM_ACL_MAX + 1];
	rdk_cm_acl_t *acl;
	uint32_t n, i, round, hits = 0;
	ipaddr_t a;

	/* Addresses that are not one unicast peer. */
	CHECK(create1(0) == EINVAL);
	CHECK(create1(0x00000001) == EINVAL);
	CHECK(create1(0xffffffff) == EINVAL);
	CHECK(create1(0x7f000001) == EINVAL);
	CHECK(create1(0xe0000001) == EINVAL);
	CHECK(create1(0xefffffff) == EINVAL);
	CHECK(create1(0x0a0b0001) == 0);
	CHECK(create1(0xdfffffff) == 0);
	sin_set(&peers[0], 0x0a000001);
	peers[0].sin_family = AF_INET6;
	CHECK(rdk_cm_acl_create(peers, 1, &acl) == EINVAL && acl == NULL);
	CHECK(rdk_cm_acl_create(NULL, 1, &acl) == EINVAL && acl == NULL);
	CHECK(rdk_cm_acl_create(peers, 0, &acl) == EINVAL && acl == NULL);
	for (i = 0; i <= RDK_CM_ACL_MAX; i++)
		sin_set(&peers[i], 0x0a000001 + i);
	CHECK(rdk_cm_acl_create(peers, RDK_CM_ACL_MAX + 1, &acl) == EINVAL);
	/* One bad entry anywhere refuses the whole list. */
	sin_set(&peers[RDK_CM_ACL_MAX - 1], 0xe0000001);
	CHECK(rdk_cm_acl_create(peers, RDK_CM_ACL_MAX, &acl) == EINVAL);

	for (round = 0; round < 2000; round++) {
		n = 1 + rnd() % (round < 20 ? RDK_CM_ACL_MAX : 64);
		for (i = 0; i < n; i++) {
			/* A small space so that lists repeat addresses. */
			sin_set(&peers[i], 0x0a000001 + rnd() % 512 +
			    ((rnd() & 1) ? 0x40000000 : 0));
		}
		CHECK(rdk_cm_acl_create(peers, n, &acl) == 0);
		CHECK(acl->rca_n == n && acl->rca_refs == 1);
		for (i = 1; i < n; i++) {
			CHECK(ntohl(acl->rca_addr[i - 1]) <=
			    ntohl(acl->rca_addr[i]));
		}
		for (i = 0; i < 2048; i++) {
			a = htonl(0x0a000000 + rnd() % 1024 +
			    ((rnd() & 1) ? 0x40000000 : 0));
			CHECK(rdk_cm_acl_allows(acl, a) ==
			    listed(peers, n, a));
			hits += rdk_cm_acl_allows(acl, a) ? 1 : 0;
		}
		for (i = 0; i < n; i++)
			CHECK(rdk_cm_acl_allows(acl, peers[i].sin_addr.s_addr));
		CHECK(!rdk_cm_acl_allows(acl, 0));
		kmem_free(acl, sizeof (*acl) + n * sizeof (ipaddr_t));
	}
	CHECK(hits != 0);
	(void) printf("acl: refusals and 2000 random lists checked\n");
	return (0);
}
