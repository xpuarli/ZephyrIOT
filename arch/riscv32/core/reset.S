/*
 * Copyright (c) 2016 Jean-Paul Etienne <fractalclone@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _ASMLANGUAGE
#include <kernel_structs.h>

/* exports */
GTEXT(__start)
GTEXT(__reset)

/* imports */
GTEXT(_PrepC)

#if CONFIG_INCLUDE_RESET_VECTOR
SECTION_FUNC(reset, __reset)
	/*
	 * jump to __start
	 * use call opcode in case __start is far away.
	 * This will be dependent on linker.ld configuration.
	 */
	call __start
#endif /* CONFIG_INCLUDE_RESET_VECTOR */

/* use ABI name of registers for the sake of simplicity */

/*
 * Remainder of asm-land initialization code before we can jump into
 * the C domain
 */
SECTION_FUNC(TEXT, __start)
#ifdef CONFIG_INIT_STACKS
	/* Pre-populate all bytes in _interrupt_stack with 0xAA */
	la t0, _interrupt_stack
	li t1, CONFIG_ISR_STACK_SIZE
	add t1, t1, t0

	/* Populate _interrupt_stack with 0xaaaaaaaa */
	li t2, 0xaaaaaaaa
aa_loop:
	sw t2, 0x00(t0)
	addi t0, t0, 4
	blt t0, t1, aa_loop
#endif

	/*
	 * Initially, setup stack pointer to
	 * _interrupt_stack + CONFIG_ISR_STACK_SIZE
	 */
	la sp, _interrupt_stack
	li t0, CONFIG_ISR_STACK_SIZE
	add sp, sp, t0

	/*
	 * Jump into C domain. _PrepC zeroes BSS, copies rw data into RAM,
	 * and then enters kernel _Cstart
	 */
	call _PrepC
