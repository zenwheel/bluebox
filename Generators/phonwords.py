#!/usr/bin/env python3
"""Generate Sources/Toolbox/phonwords_gen.c: English words by their sounds.

Run from the repository root:

    python3 Generators/phonwords.py [cache-dir]

Programs that spoke with MacinTalk often gave it their own phonemes (the
Talking Moose's phrases are all hand-written phonemes) and the Speech
Manager takes them too ([[inpt PHON]]), but today's voices read phonemes
out as letters.  Blue Box turns them back into words (phonwords.c), with
this table: the pronunciations of the CMU Pronouncing Dictionary in the
Speech Manager's phoneme code, for the words of SCOWL's commoner lists,
each with how common the word is (SCOWL's size level).

Both are fetched here (pinned versions, into cache-dir, by default a
temporary directory) and never copied by hand: CMUdict under its BSD-style
licence, SCOWL under Kevin Atkinson's permissive one.  Both notices go into
the generated file and into Resources/Words/LICENSE.txt, which the app
carries.
"""
import io
import os
import sys
import tarfile
import tempfile
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "Sources", "Toolbox", "phonwords_gen.c")
NOTICES = os.path.join(ROOT, "Resources", "Words", "LICENSE.txt")

CMUDICT_COMMIT = "74790861f652b15e4ac49015a90074ad62a27690"
CMUDICT = "https://raw.githubusercontent.com/cmusphinx/cmudict/%s/" % CMUDICT_COMMIT
SCOWL_VERSION = "2020.12.07"
SCOWL = "https://downloads.sourceforge.net/project/wordlist/SCOWL/%s/scowl-%s.tar.gz" % (SCOWL_VERSION, SCOWL_VERSION)

# The Speech Manager's phoneme opcodes (speech.c's PH_...), 2 to 41.
OPCODES = ["AE", "EY", "AO", "AX", "IY", "EH", "IH", "AY", "IX", "AA", "UW", "UH", "UX", "OW", "AW", "OY",
           "b", "C", "d", "D", "f", "g", "h", "J", "k", "l", "m", "n", "N", "p", "r", "s", "S", "t", "T",
           "v", "w", "y", "z", "Z"]
# How the table writes each (one character; phonwords.c has the same string).
CODE = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#$&*"
assert len(CODE) == len(OPCODES)

# ARPAbet (without its stress) in the Speech Manager's code.
ARPA = {
    "AA": ["AA"], "AE": ["AE"], "AO": ["AO"], "AW": ["AW"], "AY": ["AY"], "EH": ["EH"], "EY": ["EY"],
    "IH": ["IH"], "IY": ["IY"], "OW": ["OW"], "OY": ["OY"], "UH": ["UH"], "UW": ["UW"],
    "B": ["b"], "CH": ["C"], "D": ["d"], "DH": ["D"], "F": ["f"], "G": ["g"], "HH": ["h"], "JH": ["J"],
    "K": ["k"], "L": ["l"], "M": ["m"], "N": ["n"], "NG": ["N"], "P": ["p"], "R": ["r"], "S": ["s"],
    "SH": ["S"], "T": ["t"], "TH": ["T"], "V": ["v"], "W": ["w"], "Y": ["y"], "Z": ["z"], "ZH": ["Z"],
}

# SCOWL lists and the commonness level each gives (0 the commonest).
LEVELS = [(10, 0), (20, 1), (35, 2), (40, 3), (50, 3), (55, 4), (60, 4)]
KINDS = ["words", "contractions", "upper", "proper-names"]
SPELLINGS = ["english", "american"]


def fetch(url, cache, name):
    path = os.path.join(cache, name)
    if not os.path.exists(path):
        print("fetching", url)
        with urllib.request.urlopen(url) as r:
            data = r.read()
        with open(path, "wb") as f:
            f.write(data)
    with open(path, "rb") as f:
        return f.read()


def sm_phonemes(arpa):
    out = []
    for p in arpa:
        base, stress = p.rstrip("012"), p[len(p.rstrip("012")):]
        if base == "AH":
            out.append("AX" if stress == "0" else "UX")
        elif base == "ER":
            out += ["AX" if stress == "0" else "UX", "r"]
        else:
            out += ARPA[base]
    # The same sound twice is one (phonwords.c does the same to its input).
    collapsed = []
    for p in out:
        if not collapsed or collapsed[-1] != p:
            collapsed.append(p)
    return collapsed


def main():
    cache = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp(prefix="phonwords-")
    os.makedirs(cache, exist_ok=True)
    cmu = fetch(CMUDICT + "cmudict.dict", cache, "cmudict.dict").decode("latin-1")
    cmu_licence = fetch(CMUDICT + "LICENSE", cache, "cmudict.LICENSE").decode("latin-1")
    scowl_tar = fetch(SCOWL, cache, "scowl-%s.tar.gz" % SCOWL_VERSION)

    level = {}
    scowl_copyright = ""
    with tarfile.open(fileobj=io.BytesIO(scowl_tar)) as tar:
        for m in tar.getmembers():
            base = os.path.basename(m.name)
            if m.name.endswith("/Copyright") and m.name.count("/") == 1:
                scowl_copyright = tar.extractfile(m).read().decode("latin-1")
            if "/final/" not in m.name:
                continue
            for size, lv in LEVELS:
                for sp in SPELLINGS:
                    for kind in KINDS:
                        if base == "%s-%s.%d" % (sp, kind, size):
                            for w in tar.extractfile(m).read().decode("latin-1").split("\n"):
                                w = w.strip()
                                # Abbreviations (all capitals) aren't said as words.
                                if not w or (len(w) > 1 and w.isupper()):
                                    continue
                                w = w.lower()
                                if all("a" <= c <= "z" or c == "'" for c in w):
                                    level[w] = min(level.get(w, 9), lv)
    assert scowl_copyright, "no Copyright in the SCOWL archive"

    # Every word's pronunciations (a word's first is CMUdict's own, the
    # others are its variants, marked by their level plus 5).
    pairs = {}
    seen_word = set()
    for line in cmu.split("\n"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        parts = line.split()
        word = parts[0].split("(")[0].lower()
        if word not in level:
            continue
        try:
            ph = sm_phonemes(parts[1:])
        except KeyError:
            continue
        if not any(p[0].isupper() and len(p) == 2 for p in ph):
            continue  # no vowel ('s, z as "zz"): not a word to find in phonemes
        code = "".join(CODE[OPCODES.index(p)] for p in ph)
        lv = level[word] + (5 if word in seen_word else 0)
        seen_word.add(word)
        if (code, word) not in pairs or lv < pairs[(code, word)]:
            pairs[(code, word)] = lv
    entries = sorted((code, (lv, len(word), word)) for (code, word), lv in pairs.items())

    # Pronunciations front-coded: a lowercase letter, the count of the
    # phonemes shared with the one before ('a' none), then the rest; the
    # words in the same order, front-coded the same way with a capital
    # ('A' none), each ended by its level as a digit.  Words that sound the
    # same follow each other, the commonest first.
    def shared(a, b):
        k = 0
        while k < len(a) and k < len(b) and k < 25 and a[k] == b[k]:
            k += 1
        return k

    pron, words = [], []
    prev, prev_word = "", ""
    for code, (lv, _, word) in entries:
        k = shared(prev, code)
        pron.append(chr(ord("a") + k) + code[k:])
        k = shared(prev_word, word)
        words.append(chr(ord("A") + k) + word[k:] + str(lv))
        prev, prev_word = code, word

    def c_string(name, parts):
        lines, cur = [], ""
        for p in parts:
            if len(cur) + len(p) > 100:
                lines.append('    "%s"' % cur)
                cur = ""
            cur += p
        if cur:
            lines.append('    "%s"' % cur)
        return "const char %s[] =\n%s;\n" % (name, "\n".join(lines))

    def comment(text):
        return "".join((" * " + l).rstrip() + "\n" for l in text.strip("\n").split("\n"))

    with open(OUT, "w") as f:
        f.write("/* Generated by Generators/phonwords.py; do not edit.\n *\n")
        f.write(" * English words and their sounds in the Speech Manager's phoneme code,\n")
        f.write(" * %d of them: the pronunciations of the CMU Pronouncing Dictionary\n" % len(entries))
        f.write(" * (github.com/cmusphinx/cmudict, %s) for the words of SCOWL\n" % CMUDICT_COMMIT[:12])
        f.write(" * %s (wordlist.aspell.net) to size %d, under these notices:\n *\n" % (SCOWL_VERSION, LEVELS[-1][0]))
        f.write(" * ---- CMUdict ----\n")
        f.write(comment(cmu_licence))
        f.write(" *\n * ---- SCOWL ----\n")
        f.write(comment(scowl_copyright.replace("*/", "* /")))
        f.write(" */\n#include \"phonwords.h\"\n\n")
        f.write("const char phw_codes[] = \"%s\";\n" % CODE)
        f.write("const int phw_count = %d;\n" % len(entries))
        f.write(c_string("phw_pron", pron))
        f.write(c_string("phw_words", [w.replace("'", "\\'") for w in words]))
    os.makedirs(os.path.dirname(NOTICES), exist_ok=True)
    with open(NOTICES, "w") as f:
        f.write("Blue Box turns phonemes back into words with a table made from these word lists\n")
        f.write("(Sources/Toolbox/phonwords_gen.c, by Generators/phonwords.py).\n\n")
        f.write("The CMU Pronouncing Dictionary (github.com/cmusphinx/cmudict, %s)\n\n" % CMUDICT_COMMIT)
        f.write(cmu_licence.strip() + "\n\n")
        f.write("SCOWL %s (wordlist.aspell.net), sizes 10 to %d\n\n" % (SCOWL_VERSION, LEVELS[-1][0]))
        f.write(scowl_copyright.strip() + "\n")
    print("%d entries, %d bytes of sounds, %d of words" % (len(entries), sum(map(len, pron)), sum(map(len, words))))


if __name__ == "__main__":
    main()
