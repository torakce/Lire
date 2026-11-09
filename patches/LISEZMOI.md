# Correctifs de performance appliqués à MuPDF 1.28.6

`build.sh` applique automatiquement `mupdf-1.28.6-lire.patch` aux sources officielles de MuPDF.
Ces modifications sont publiées ici conformément à la licence AGPL v3 de MuPDF.

| Fichier | Changement | Effet mesuré |
|---|---|---|
| `source/fitz/draw-device.c` | **Tracé direct des traits fins et moyens** : un trait d'au plus 1 px (toute couleur), ou d'au plus 3 px (opaque), est tracé segment par segment avec une couverture calculée exactement, au lieu de passer par le convertisseur général dont le coût croît comme *nombre d'arêtes du chemin × hauteur en sous-lignes*. Tirets, bouts et jonctions gérés. | Plan 4A0 très chargé, vue entière : 8,0 s → 0,9 s (1 cœur). Zoom ×8 : 3,0 s → 0,35 s. Rendu visuellement identique. |
| `source/fitz/list-device.c` | Relecture d'une display list **sans compteur de références** (la liste garde ses objets vivants) : plus de verrou global pris 2 à 6 fois par élément. | Parcours 2× plus rapide, et les threads de rendu ne se gênent plus. |
| `source/fitz/draw-glyph.c`, `context.c/.h`, `glyph-imp.h` | **Cache de glyphes propre à chaque thread**, consulté sans verrou avant le cache partagé. | Page de texte, 2 threads : 3,5× plus lent qu'un seul → 1,9× plus rapide. |
| `source/pdf/pdf-lex.c` | Conversion directe des nombres réels simples (`-123.456`) au lieu de `fz_atof`. | Analyse d'une page ~17 % plus rapide (résultat identique au pixel près). |
| `include/mupdf/fitz/stream.h`, `stream-read.c` | Lecture d'un octet réellement *inline* (le `fz_try` empêchait l'inlining). | Analyse ~12 % plus rapide. |
| `list-device.c`, `display-list.h` | `fz_display_list_memory()` : taille d'une display list, pour gérer le budget mémoire. | — |

Pour comparer avec le rendu d'origine : variable d'environnement `LIRE_NOHL=1` (désactive le tracé direct).
