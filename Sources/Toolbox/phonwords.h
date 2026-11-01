/*
 * Phonemes back into words (phonwords.c) and the table it uses
 * (phonwords_gen.c, made by Generators/phonwords.py).
 */
#ifndef PHONWORDS_H
#define PHONWORDS_H

#include <stddef.h>
#include <stdint.h>

/* The MacinTalk phoneme opcodes (American English), the Speech Manager's. */
enum {
    PH_SILENCE, PH_BREATH, PH_AE, PH_EY, PH_AO, PH_AX, PH_IY, PH_EH, PH_IH, PH_AY, PH_IX, PH_AA, PH_UW, PH_UH,
    PH_UX, PH_OW, PH_AW, PH_OY, PH_b, PH_C, PH_d, PH_D, PH_f, PH_g, PH_h, PH_J, PH_k, PH_l, PH_m, PH_n, PH_N,
    PH_p, PH_r, PH_s, PH_S, PH_t, PH_T, PH_v, PH_w, PH_y, PH_z, PH_Z,
    PH_COUNT
};

/* The table: phw_count pronunciations, each a run of phw_codes letters
 * (opcode PH_AE onwards) after a lowercase letter that says how many it
 * shares with the one before; the words in the same order, each after a
 * capital saying the same of its letters and before a digit, its level
 * (0 the commonest words to 4; plus 5: not the word's first sound). */
extern const char phw_codes[];
extern const int phw_count;
extern const char phw_pron[];
extern const char phw_words[];

/* Phonemes (the Speech Manager's symbols, as [[inpt PHON]] takes them,
 * prosody marks and all) as English text, NUL-ended; its length.  from
 * and to (either may be NULL) get, for each byte of the text, the part of
 * the input it came from. */
size_t phonemes_to_text(const uint8_t *in, size_t n, char *out, size_t outsize, uint32_t *from, uint32_t *to);
/* What the last phonemes_to_text found: words that matched exactly, words
 * that nearly did, and runs of sounds spelt out. */
void phonemes_to_text_stats(int *exact, int *near, int *spelled);
/* A word's phonemes (opcodes) from the table; 0 if it isn't there. */
int phonwords_word_phonemes(const uint8_t *text, int len, uint8_t *out, int max);

#endif
