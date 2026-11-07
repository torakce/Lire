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
| Défiler | Molette, flèches, Espace / Maj+Espace |
| Zoomer / dézoomer (autour du curseur) | Ctrl+Molette, `+` / `-` |
| Zoom sur une zone | Clic droit + glisser un rectangle |
| Déplacer la vue | Clic gauche (ou milieu) + glisser |
| Page entière / pleine largeur | `F` ou `Ctrl+0` / `L` ou `Ctrl+2` |
| Taille réelle (100 %) | `Ctrl+1` |
| Page précédente / suivante | `Pg.Préc` / `Pg.Suiv` (et ← / → quand la page tient en largeur) |
| Première / dernière page | `Début` / `Fin` |
| Aller à la page | `Ctrl+G` ou taper directement le numéro (pavé numérique) |
| Rotation horaire / antihoraire | Boutons ↻ / ↺ en haut à droite, `R` / `Maj+R`, ou `Ctrl+Maj++` / `Ctrl+Maj+-` |
| Molette = zoom ↔ défilement | `M` (mémorisé) |
| Recharger le fichier (après un nouvel export) | `F5` |
| Plein écran | `F11` |
| Aide | `F1` ou `H` |

La barre de défilement de droite est saisissable ; en la faisant glisser, le numéro de page s'affiche.
La rotation s'applique à tout le document de l'onglet ; l'angle courant est rappelé dans la barre d'état.
Les PDF protégés par mot de passe sont pris en charge.

## Pourquoi c'est rapide

1. **Moteur MuPDF** : le moteur de rendu PDF le plus rapide disponible en open source.
2. **Chaque page est analysée une seule fois** (thread dédié) en une *display list* en mémoire.
   Zoomer ou se déplacer ne ré-interprète jamais le PDF : on rejoue la liste.
3. **Rendu en tuiles de 512 px, en parallèle sur tous les cœurs**, uniquement pour la zone visible
   (+ une couronne d'anticipation), au facteur de zoom exact de l'écran → rendu net, pas d'image géante en mémoire.
4. **Zoom sans attente** : la carte graphique (OpenGL) ré-étire instantanément ce qui est déjà calculé,
   puis les tuiles nettes arrivent, du centre vers les bords. Un rendu devenu inutile (on a re-zoomé, on s'est
   déplacé) est interrompu immédiatement.
5. **Pages voisines pré-calculées** : quand la page tient à l'écran, la suivante et la précédente sont préparées
   en tâche de fond → `Pg.Suiv` est instantané.
6. Lissage des traits sur 64 niveaux (au lieu de 256) : visuellement identique, ~1,6× plus rapide.
   Gestion ICC désactivée (inutile pour des plans, coûteuse).
7. Cache de tuiles et de pages limité (≈ 384 Mo graphiques, 8 pages analysées hors écran) pour rester léger.

### Mesures (environnement de test à 2 cœurs lents, donc pessimistes)

| Fichier de test | Ouverture | 1er affichage net | Zoom ×5 net | Déplacement net | Page suivante |
|---|---|---|---|---|---|
| Plan 4A0 (6741×4768 pt), 1,2 M traits + 40 000 textes | 0,2 ms | 2,7 s | 0,95 s | 0,11 s | — |
| Liasse 10 × A0 (150 000 traits chacun) | 0,3 ms | 17 ms (page A4) | 10 ms | — | **0 ms** (pré-calculée) |
| Document texte 400 pages | 1,7 ms | 17 ms | 9 ms | — | **0,1 ms** |

Pendant un zoom ou un déplacement, l'image est recomposée en < 3 ms (aucun blocage de l'interface).
Sur un PC de bureau à 8 cœurs, compter 3 à 6 fois plus rapide pour les rendus.

## Structure du code

```
src/core.c     moteur portable : onglets, documents, mise en page, tuiles, threads, cache, zoom, navigation, interface
src/gfx_gl.c   affichage OpenGL 1.1 (fonctionne sur tous les Windows)
src/win32.c    fenêtre Windows, instance unique, clavier/souris, glisser-déposer, réglages (HKCU\Software\Lire)
src/thread.h   threads Win32 / POSIX
res/           icône, manifeste (haute résolution par écran), informations de version
tests/         banc de test sans fenêtre (Linux), générateur de PDF lourds, mesures MuPDF
build.sh       compilation croisée Linux → Lire.exe
```

## Compiler

Depuis Linux (ou WSL) avec `mingw-w64` : `./build.sh` → `build/Lire.exe`.

## Licence

Lire utilise MuPDF (Artifex), distribué sous licence **AGPL v3** ; Lire.exe l'est donc aussi.
Usage personnel ou interne : aucune contrainte. Si tu redistribues l'exe, fournis aussi ce code source.
