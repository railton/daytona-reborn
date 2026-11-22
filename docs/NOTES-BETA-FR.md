# Daytona Arcade Native : bêta 0.9

Première version publique, pour Windows. Installation :
[INSTALLATION-FR.md](INSTALLATION-FR.md).

## D'où vient le projet

Daytona Arcade Native s'appuie sur la recompilation statique
[daytona-arcade-recomp](https://github.com/alphanu1/daytona-arcade-recomp)
d'alphanu1. Le programme de la borne y est traduit en code C++ natif, sans
émulateur. Cette édition garde la simulation d'origine, donc la conduite et
la course de la borne, et travaille sur l'affichage et le confort de jeu sur
PC.

## Nouveautés de cette édition

- Un menu PC : course normale ou Time Attack, boîte automatique ou
  manuelle, réglages.
- L'écran large (16:10, 16:9, 21:9). Le décor et les voitures sont visibles
  sur les côtés.
- Une distance d'affichage réglable. Le pont d'Expert est visible dès le
  départ.
- Un décor en volume, en option : sapins, tribunes, piliers du pont de
  Dinosaur Canyon.
- Une courbe d'écran façon borne à tube, et un réglage de saturation.
- Les voitures adverses toujours détaillées, en option : un seul modèle à
  toutes les distances, sans changement d'aspect en approchant.
- Le lissage 120 images/s, en option expérimentale.

## Limites connues

- **Lissage 120 images/s** : des voitures adverses peuvent trembler ou se
  dédoubler, surtout après quelques tours. L'option est coupée par défaut,
  demande le rendu Matériel, et la correction est en cours.
- **Voitures adverses** : ce sont les modèles d'origine de la borne. Une
  reconstruction plus détaillée est prévue, fidèle aux modèles d'origine.
- **Ray tracing** : à l'étude, pour une version ultérieure.
- Seule la ROM *Daytona USA* Revision A (`daytona`) donne accès au menu PC
  et aux améliorations.

## Aucune donnée du jeu

Le dépôt ne contient ni ROM ni code tiré de la ROM. Le jeu est reconstruit
sur votre ordinateur à partir de votre propre copie.
