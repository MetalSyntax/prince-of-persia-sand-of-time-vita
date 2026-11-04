/* Idioma del juego (-Wl,--wrap=resot_game_language). Desde rePOP 0.5.0 el kernel
 * (ExQueryNonVolatileSetting) y el hook de XGetLanguage (target_hooks.c, sub_00369310)
 * preguntan a resot_game_language(), que en options_android.c vale 1 (inglés) salvo el INI.
 * RECOMP_XC_LANGUAGE en env.txt: 1 inglés, 4 francés, 5 español (valores XC_LANGUAGE del
 * Xbox); el hook lo pasa a 0/1/2, que elige soundxN, POPLOGON, POP_INN, POP_OUTN y CINE_90xN. */
#include <stdlib.h>

int __real_resot_game_language(void);

int __wrap_resot_game_language(void)
{
    const char *lang = getenv("RECOMP_XC_LANGUAGE");
    return lang && atoi(lang) ? atoi(lang) : __real_resot_game_language();
}
