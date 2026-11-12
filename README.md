# Lire — visionneuse PDF rapide et légère

Lecture seule, pensée pour les **plans mécaniques lourds** (formats A0 et plus, des centaines de milliers de traits).
Un seul fichier `Lire.exe`, sans installation, sans dépendance.

## Utilisation

- Double-clic sur `Lire.exe`, puis glisser-déposer un ou plusieurs PDF dans la fenêtre (ou bouton `+` / `Ctrl+O`, sélection multiple possible).
- Ou : clic droit sur un PDF → *Ouvrir avec* → `Lire.exe` (cocher « toujours utiliser » pour l'associer).
  Si Lire est déjà ouvert, le fichier s'ajoute **dans un nouvel onglet** de la fenêtre existante.
- Ou en ligne de commande : `Lire.exe "C:\plans\pièce 123.pdf" "C:\plans\pièce 124.pdf"`.

### Onglets

Chaque fichier s'ouvre dans son propre onglet, qui garde son zoom, sa position et sa rotation.
Passer d'un onglet à l'autre est instantané : l'onglet mis en arrière-plan conserve ce qui était à l'écran
(et libère le reste pour rester léger). Rouvrir un fichier déjà ouvert active simplement son onglet.

| Action | Raccourci |
|---|---|
| Onglet suivant / précédent | `Ctrl+Tab` / `Ctrl+Maj+Tab` (ou `Ctrl+Pg.Suiv` / `Ctrl+Pg.Préc`) |
| Fermer l'onglet | `Ctrl+W`, clic sur `×` ou clic molette sur l'onglet |
| Rouvrir le dernier onglet fermé | `Ctrl+Maj+T` |
| Ouvrir le fichier avec un autre logiciel (Acrobat, Annoter…) | Bouton en haut à droite ou `Ctrl+Maj+O` |
| Défiler | Molette, flèches, Espace / Maj+Espace |
| Zoomer / dézoomer (autour du curseur) | Ctrl+Molette, `+` / `-` |
| Zoom sur une zone | Clic droit + glisser un rectangle |
| Déplacer la vue | Clic gauche + glisser (hors du texte) |
| Sélectionner du texte | Clic gauche + glisser **sur** le texte (comme Acrobat) ; double-clic : un mot ; `Maj`+clic : étendre |
| Copier le texte sélectionné / tout le texte de la page | `Ctrl+C` / `Ctrl+A` puis `Ctrl+C` |
| Rechercher du texte dans tout le document | `Ctrl+F`, taper (ou coller avec `Ctrl+V`) ; `Entrée` / `Maj+Entrée` : résultat suivant / précédent (aussi `F3` / `Maj+F3` ou les flèches de la barre) ; `Échap` : fermer |
| Défilement automatique | Maintenir le clic molette : la vue file dans la direction du curseur, d'autant plus vite qu'il est loin du point de départ ; elle s'arrête au relâchement |
| Page entière / pleine largeur | `F` ou `Ctrl+0` / `L` ou `Ctrl+2` |
| Taille réelle (100 %) | `Ctrl+1` |
| Page précédente / suivante | `Pg.Préc` / `Pg.Suiv` (et ← / → quand la page tient en largeur) |
| Première / dernière page | `Début` / `Fin` |
| Aller à la page | `Ctrl+G` ou taper directement le numéro (pavé numérique) |
| Une page ↔ deux pages côte à côte | Bouton « Affichage » en haut à droite, ou `D` |
| Page de garde séparée (en deux pages) | Bouton « Affichage », ou `Maj+D` |
| Défilement continu ↔ page par page | Bouton « Affichage », ou `C` |
| Rotation horaire / antihoraire | Boutons ↻ / ↺ en haut à droite, `R` / `Maj+R`, ou `Ctrl+Maj++` / `Ctrl+Maj+-` |
| Molette = zoom ↔ défilement | `M` (mémorisé) |
| Recharger le fichier (après un nouvel export) | `F5` |
| Plein écran | `F11` |
| Aide | `F1` ou `H` |

Modes d'affichage (mémorisés d'une session à l'autre) : **deux pages** côte à côte, avec en option la **page de garde
séparée** (la première page reste seule, à droite, puis 2–3, 4–5…) ; **continu** (toutes les pages défilent) ou
**page par page** (une page ou une double page à la fois : arrivé en bas, la molette passe à la suivante).
« Page entière » et « pleine largeur » s'ajustent à la double page.

« Ouvrir avec » propose les logiciels que Windows recommande pour les PDF, puis ceux déjà choisis via
« Choisir un programme… » (pratique pour un logiciel portable comme Annoter, que Windows ne connaît pas) ;
« Autres applications (Windows)… » ouvre la fenêtre standard, sans jamais changer le logiciel par défaut.

Sélection de texte : le curseur devient une barre verticale au-dessus d'un texte. Le texte de chaque page affichée est
extrait en tâche de fond depuis sa display list (quelques dizaines de ms, sans effet sur l'affichage). Si un plan
a été exporté avec les textes « vectorisés » (transformés en traits), il n'y a pas de texte à sélectionner.

Recherche (`Ctrl+F`) : la barre s'ouvre en haut à droite et la recherche se fait **pendant la frappe**, dans tout le
document, en partant de la page affichée. Elle ignore majuscules et accents (« tolerance » trouve « Tolérance »).
Tous les résultats sont surlignés en jaune, le résultat courant en orange, avec le compteur « 3 / 57 ».
Sur un plan affiché en entier, où le texte est illisible, Lire zoome juste ce qu'il faut pour lire le résultat
(et l'encadre) ; sinon le zoom ne change pas. Zoom, rotation, défilement et souris restent utilisables barre ouverte.
La recherche est linéaire (une page A0 chargée de cotes est parcourue en ~10 ms) et faite par tranches entre deux
images : l'interface reste fluide, même sur un document de plusieurs centaines de pages. Comme pour la sélection,
un texte « vectorisé » à l'export n'est pas cherchable.

La barre de défilement de droite est saisissable ; en la faisant glisser, le numéro de page s'affiche.
La rotation s'applique à tout le document de l'onglet ; l'angle courant est rappelé dans la barre d'état.
Les PDF protégés par mot de passe sont pris en charge.

## Pourquoi c'est rapide

1. **Moteur MuPDF**, avec des correctifs de performance propres à Lire (voir [`patches/`](patches/LISEZMOI.md)) :
   - **tracé direct des traits fins et moyens** (≤ 3 px à l'écran, soit l'essentiel d'un plan) :
     8 à 10 fois plus rapide que le convertisseur général, rendu identique ;
   - **les threads de rendu ne se bloquent plus entre eux** (plus de verrou global par trait ni par caractère) :
     le rendu se répartit vraiment sur tous les cœurs ;
   - **analyse du PDF ~30 % plus rapide** (lecture des nombres et des octets).
2. **Seule la page affichée est analysée d'abord** ; toutes les autres le sont ensuite en tâche de fond,
   par plusieurs threads en parallèle (chacun avec sa propre instance du fichier), des plus proches aux plus lointaines.
   Chaque page n'est analysée qu'une fois, en une *display list* gardée en mémoire : zoomer ou se déplacer
   ne ré-interprète jamais le PDF.
3. **Rendu en tuiles de 512 px, en parallèle sur tous les cœurs.**
4. **Niveaux de zoom alignés sur la molette** : chaque cran (×1,26) tombe pile sur un niveau de tuiles ;
   revenir en arrière ne recalcule rien. Page entière, 100 % et zoom sur zone restent rendus au pixel près.
5. **Anticipation** : dès que l'écran est net, Lire calcule en tâche de fond la couronne autour de l'écran,
   le cran de zoom suivant **et** le précédent, puis le reste de la page au zoom courant, et les pages voisines.
   Résultat : un cran de molette, un déplacement ou `Pg.Suiv` sont nets **immédiatement**.
6. **Zoom sans attente** : en attendant, la carte graphique (OpenGL) ré-étire ce qui est déjà calculé.
   Un rendu devenu inutile est interrompu immédiatement.
7. **Mémoire maîtrisée** : les images calculées d'avance sont gardées **compressées** (un plan, ce sont des traits
   sur fond blanc : niveaux de gris sur 1 octet au lieu de 4, zones blanches gratuites, plages unies regroupées) ;
   seules celles qui sont à l'écran, ou sur le point d'y être, deviennent des textures pour la carte graphique.
   La quantité d'anticipation reste proportionnelle à la RAM (≈ 1500 tuiles sur un PC à 32 Go), mais occupe
   3 à 9 fois moins de mémoire qu'avant ; le rendu est identique au pixel près. Pages analysées : ≈ 12 % de la RAM
   (max 3 Go). `F1` affiche en bas de l'aide où part la mémoire.
8. Lissage des traits sur 64 niveaux, gestion ICC désactivée (inutile pour des plans, coûteuse).

### Mesures (environnement de test à 2 cœurs lents, donc pessimistes)

Délai avant que la vue soit **entièrement nette**, en laissant à Lire le temps d'anticiper entre deux actions,
comme le ferait quelqu'un qui regarde le plan :

| Action | Plan 4A0 très chargé (1,2 M traits) — v1.1 | v1.2 | Plan A0 (liasse) — v1.1 | v1.2 |
|---|---|---|---|---|
| Ouverture → page nette | 4,0 s | **1,0 s** | 1,5 s | **0,3 s** |
| 1 cran de molette (avant) | 3,4 à 4,1 s | **instantané** | 0,8 à 1,0 s | **instantané** |
| 1 cran de molette (arrière) | instantané | instantané | instantané | instantané |
| 3 crans d'un coup | 2,2 s | **0,4 s** | 0,5 s | **0,05 s** |
| Retour page entière | instantané | instantané | 1,0 s | **0,09 s** |
| **Total des attentes du scénario** | **17,5 s** | **1,4 s** | **7,3 s** | **0,6 s** |

Mémoire du processus après un balayage rapide à fort zoom, avec le budget d'un PC à 32 Go (banc de test) :

| Document | v1.5.0 | v1.5.1 |
|---|---|---|
| Plan A0 de densité courante | 1,7 Go | **0,19 Go** |
| Liasse de 10 plans A0 très chargés | 1,9 Go | **0,54 Go** |
| Plan 4A0 extrêmement chargé (1,2 M traits) | 1,2 Go | **0,46 Go** |

Rendu d'une vue 2560×1440 du plan 4A0 sur un seul cœur : page entière 8,0 s → 0,9 s, zoom ×8 3,0 s → 0,35 s.
Le rendu se répartit maintenant sur tous les cœurs : sur un PC de bureau à 8–16 cœurs, compter 4 à 8 fois mieux.
Pendant un zoom ou un déplacement, l'image est recomposée en < 3 ms (aucun blocage de l'interface).

## Structure du code

```
src/core.c     moteur portable : onglets, documents, mise en page, tuiles, threads, cache, zoom, navigation, interface
src/gfx_gl.c   affichage OpenGL 1.1 (fonctionne sur tous les Windows)
src/win32.c    fenêtre Windows, instance unique, clavier/souris, glisser-déposer, réglages (HKCU\Software\Lire)
src/thread.h   threads Win32 / POSIX
res/           icône, manifeste (haute résolution par écran), informations de version
patches/       correctifs de performance appliqués à MuPDF (avec leur explication)
tests/         banc de test sans fenêtre (Linux), générateur de PDF lourds, mesures MuPDF
build.sh       compilation croisée Linux → Lire.exe (télécharge MuPDF et applique les correctifs)
```

## Compiler

Depuis Linux (ou WSL) avec `mingw-w64` : `./build.sh` → `build/Lire.exe`.

## Licence

Lire utilise MuPDF (Artifex), distribué sous licence **AGPL v3**, avec les modifications publiées dans `patches/` ;
Lire.exe l'est donc aussi.
Usage personnel ou interne : aucune contrainte. Si tu redistribues l'exe, fournis aussi ce code source.
