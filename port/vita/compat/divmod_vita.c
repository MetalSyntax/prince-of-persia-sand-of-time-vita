/* División de 64 bits rápida cuando cabe en 32 (-Wl,--wrap=__aeabi_ldivmod,--wrap=__aeabi_uldivmod).
 *
 * El código generado traduce cdq/idiv y xor edx/div como int64 / int32 (`_dividend / (int32_t)ecx`),
 * así que GCC llama a __aeabi_ldivmod -> __udivmoddi4 (libgcc, en C, sin división por hardware en
 * el Cortex-A9). Casi siempre edx es solo el signo de eax: con los dos operandos en 32 bits se usa
 * __aeabi_idivmod/__aeabi_uidivmod, con el mismo resultado. Perfil del menú en hosttest
 * (2026-11-04, sin vídeo): __udivmoddi4 6,6% del tiempo activo de game_main.
 *
 * ABI (RTABI): numerador r1:r0, divisor r3:r2; cociente r1:r0, resto r3:r2. Por eso es asm.
 * Fuera del atajo: divisor 0 (lo que haga libgcc) e INT32_MIN / -1 (desborda en 32 bits).
 * Prueba: hosttest/divmod_test.c. */
__asm__(
    "    .syntax unified\n"
    "    .thumb\n"
    "    .text\n"
    "    .global __wrap___aeabi_ldivmod\n"
    "    .type __wrap___aeabi_ldivmod, %function\n"
    "    .thumb_func\n"
    "__wrap___aeabi_ldivmod:\n"
    "    cmp   r1, r0, asr #31\n"          /* numerador: ¿la parte alta es el signo de la baja? */
    "    bne   1f\n"
    "    cmp   r3, r2, asr #31\n"          /* divisor, igual */
    "    bne   1f\n"
    "    cmp   r2, #0\n"
    "    beq   1f\n"
    "    cmn   r2, #1\n"                   /* divisor -1 con INT32_MIN: el cociente no cabe */
    "    bne   2f\n"
    "    cmp   r0, #0x80000000\n"
    "    beq   1f\n"
    "2:  push  {r4, lr}\n"
    "    mov   r1, r2\n"
    "    bl    __aeabi_idivmod\n"          /* r0 cociente, r1 resto */
    "    mov   r2, r1\n"
    "    asr   r3, r1, #31\n"
    "    asr   r1, r0, #31\n"
    "    pop   {r4, pc}\n"
    "1:  b.w   __real___aeabi_ldivmod\n"
    "    .size __wrap___aeabi_ldivmod, .-__wrap___aeabi_ldivmod\n"
    "\n"
    "    .global __wrap___aeabi_uldivmod\n"
    "    .type __wrap___aeabi_uldivmod, %function\n"
    "    .thumb_func\n"
    "__wrap___aeabi_uldivmod:\n"
    "    orrs  ip, r1, r3\n"               /* las dos partes altas a 0 */
    "    bne   1f\n"
    "    cmp   r2, #0\n"
    "    beq   1f\n"
    "    push  {r4, lr}\n"
    "    mov   r1, r2\n"
    "    bl    __aeabi_uidivmod\n"
    "    mov   r2, r1\n"
    "    movs  r3, #0\n"
    "    movs  r1, #0\n"
    "    pop   {r4, pc}\n"
    "1:  b.w   __real___aeabi_uldivmod\n"
    "    .size __wrap___aeabi_uldivmod, .-__wrap___aeabi_uldivmod\n");
