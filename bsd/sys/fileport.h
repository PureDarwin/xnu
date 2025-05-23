/*
 * Copyright (c) 2010 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#ifndef _SYS_FILEPORT_H_
#define _SYS_FILEPORT_H_

#include <sys/_types.h>
#include <sys/cdefs.h>
#include <Availability.h>

#ifndef KERNEL

__BEGIN_DECLS

#if !defined(_POSIX_C_SOURCE) || defined(_DARWIN_C_SOURCE)

#ifndef _FILEPORT_T
#define _FILEPORT_T
typedef __darwin_mach_port_t fileport_t;
#define FILEPORT_NULL   ((fileport_t)0)
#endif /* _FILEPORT_T */

__API_AVAILABLE(macos(10.7), ios(4.3))
int     fileport_makeport(int, fileport_t *);

__API_AVAILABLE(macos(10.7), ios(4.3))
int     fileport_makefd(fileport_t);

#endif /* (!_POSIX_C_SOURCE || _DARWIN_C_SOURCE) */

__END_DECLS

#endif /* !KERNEL */

#endif  /* !_SYS_FILEPORT_H_ */
