/*
 * Copyright (C) 2018 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ASSERT_H
#define ASSERT_H

#ifdef HV_DEBUG
void asm_assert(int32_t line, const char *file, const char *txt);

#define ASSERT(x, ...) \
	if (!(x)) {\
		asm_assert(__LINE__, __FILE__, "fatal error");\
	}
#else
#define ASSERT(x, ...)	do { } while(0)
#endif

#endif /* ASSERT_H */
