/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * pkvm runs in a self-contained environment
 * and requires a self-contained spinlock implementation
 * which doesn't rely on any other external symbols.
 *
 * This is a common interface with wrapping the arch
 * specific implementation.
 * */
#ifndef __PKVM_SPINLOCK_H
#define __PKVM_SPINLOCK_H

#include <asm/pkvm_spinlock.h>

#endif
