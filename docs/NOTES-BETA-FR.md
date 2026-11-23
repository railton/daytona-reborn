# Daytona Arcade Native : bêta 0.10

Deuxième version publique, pour Windows. Installation :
[INSTALLATION-FR.md](INSTALLATION-FR.md). English notes:
[NOTES-BETA-EN.md](NOTES-BETA-EN.md).

## D'où vient le projet

Daytona Arcade Native s'appuie sur la recompilation statique
[daytona-arcade-recomp](https://github.com/alphanu1/daytona-arcade-recomp)
d'alphanu1. Le programme de la borne y est traduit en code C++ natif, sans
émulateur. Cette édition garde la simulation d'origine, donc la conduite et
la course de la borne, et travaille sur l'affichage et le confort de jeu sur
PC.

## Nouveautés de la 0.10

Ces nouveautés répondent aux demandes reçues sous les vidéos et sur GitHub.

- **Langue de l'interface** : ouvrez les Réglages du lanceur et utilisez
  « Langue / Language ». Français et anglais pour commencer. Le choix est gardé, et le
  premier lancement suit la langue de Windows. Le portugais, l'espagnol, le
  chinois, le japonais et l'arabe sont en préparation.
- **Format de l'image** : il est maintenant en tête des réglages
  d'affichage. Choix : **Arcade 4:3** (l'image de la borne, centrée avec des
  bandes), Automatique (la forme de la fenêtre), 16:10, 16:9 ou 21:9. Si la
  fenêtre est plus large que le format choisi, l'image est centrée avec des
  bandes latérales. Une fenêtre plus étroite limite la largeur de la vue
  en écran large.
- **Lissage 120 images/s réparé** : sur une course, 20 des 39 voitures
  adverses tremblaient ou se dédoublaient. Elles sont maintenant toutes
  suivies, et le mouvement est plus régulier. L'option reste à cocher.

## Déjà dans la 0.9

- Un menu PC : course normale ou Time Attack, boîte automatique ou
  manuelle, réglages.
- L'écran large : le décor et les voitures sont visibles sur les côtés.
- Une distance d'affichage réglable. Le pont d'Expert est visible dès le
  départ.
- Un décor en volume, en option : sapins, tribunes, piliers du pont de
  Dinosaur Canyon.
- Une courbe d'écran façon borne à tube, et un réglage de saturation.
- Les voitures adverses toujours détaillées, en option : un seul modèle à
  toutes les distances.

## Limites connues

- **Lissage 120 images/s** : il demande le rendu Matériel et le mode de
  dessin « Double tampon ». La cadence dépend de l'écran et de la carte
  graphique.
- **Voitures adverses** : ce sont les modèles d'origine de la borne. Une
  reconstruction plus détaillée est prévue.
- **Ray tracing** (ombres du soleil, lumière dans l'air) : en cours de
  réglage, il n'est pas dans cette version.
- Seule la ROM *Daytona USA* Revision A (`daytona`) donne accès au menu PC
  et aux améliorations.

## Aucune donnée du jeu

Le dépôt ne contient ni ROM ni code tiré de la ROM. Le jeu est reconstruit
sur votre ordinateur à partir de votre propre copie.
