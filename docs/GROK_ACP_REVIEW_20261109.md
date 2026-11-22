# Revue indépendante Grok via ACP — 9 novembre 2026

Agent : **Grok Build 1.0.77**, ACP `grok agent stdio`, modèle `grok-4.7`,
session `01a1c22d-d9ca-7110-8707-ee25858b44c4` (second essai). Lecture seule :
les outils de lecture ont été autorisés, les éditions et commandes mutantes
refusées. Aucun fichier du projet n'a été modifié par Grok.

## 1. Trois hypothèses fragiles du plan initial

### 1.1 « Avant `M2_GEO_POINT` = géométrie monde, `model2_3d_process_polygon` = espace écran »

Faux sur les deux espaces. `transform_point` applique une seule matrice 3×4
(`geo->matrix`, écrite en bloc par `geo_matrix_write`, translation seule par
`geo_translate_write`). La sortie est l'espace de `geo->light` : la normale passe
par `M2_GEO_VECTOR` puis `dot_product(normal, geo->light)` dans `geo_parse_np_ns`.
Aucune commande caméra ou monde séparée n'existe à côté. Ensuite `apply_focus`
multiplie x/y, puis `model2_3d_push_point` quantifie par `f2u >> 8`.
`model2_3d_process_polygon` relit ces mots et clippe ; la division perspective est
plus tard, dans `GpuRenderer::build` et `Raster::render_one`. Le hook « avant
`M2_GEO_POINT` » rate en plus la commande 02 : `geo_direct_data` pousse des points
déjà quantifiés, sans matrice ni focus.

### 1.2 « Matrice + texheader identifient objet et matériau »

Le seul identifiant de draw est `(pool, oba, obc)` dans `geo_object_data`. Le bit
`0x00800000` vise la ROM ; les autres visent `polygon_ram1`/`polygon_ram0`,
remplis par `geo_polygon_data`. La même adresse ROM est réinstanciée. Le texheader
est quatre mots lus depuis la ROM ou la texture RAM, et l'adresse avance de `tho`
à chaque polygone. `luma` n'est pas dedans : `(abs(n·l) * diffuse + ambient)` plus
le bit de face, indexé par `texture_parameters[(attr>>18)&31]`. Le GPU recharge
luma et colorxlat à chaque frame, la texture RAM seulement si `tex_generation`
change. En modes 2/3, `geo_parse_nn_ns` saute la normale du flux et la recalcule
par produit vectoriel après la matrice. Avec `M2_FAST_GEO`, UV et header ne sont
lus que si le polygone survit au culling.

### 1.3 « Un scene graph persistant interpolé à 120 Hz peut servir d'oracle pixel et recevoir un soleil »

`Geo::polys` est la liste immédiate après cull, clip (sommets créés) et
troncature `>>8`. `GpuRenderer::build` trie par fenêtre, puis clé z, puis ordre
inverse de soumission, et écrit une profondeur synthétique `depth_step`. Le test
de profondeur remplace « le premier polygone qui couvre le pixel gagne ». Un z
géométrique et une lumière directionnelle divergent de cet oracle par
construction. Entre deux ticks à 57,52 Hz, rien ne relie un sommet au suivant :
le CPU change `oba`/`obc` selon le LOD, et le link type 0 supprime des polygones.

## 2. Architecture alternative proposée par Grok

- **Second lecteur**, aucune écriture dans `Geo` ni dans le tick i960 ; le
  renderer historique continue de consommer `Geo::polys`.
- À l'entrée de `geo_object_data`, copier l'état : pool, `oba`, `obc`, `mode`,
  `matrix[12]`, focus, light, lod, les 32 `texture_parameters` et `coef_table`,
  `tpa`, `tha`, fenêtre, centre, reverse. Relire le flux à côté. La ROM se relit ;
  la polygon RAM et, si `tpa`/`tha` visent la texture RAM, ces mots-là se copient
  à cet instant.
- Chaque lien `attr & 3` produit un polygone **en espace objet** : positions avant
  `M2_GEO_POINT`, normale objet en modes 0/1, produit vectoriel des arêtes objet
  en modes 2/3, UV lus à `tpa` sans avancer le curseur matériel, quatre mots de
  header à `tha`.
- Clé géométrie ROM : `(pool, oba, obc, mode)`, mesh immutable construit une
  fois. Clé RAM : hash des mots copiés. Header et index de paramètres restent sur
  le polygone ; `luma` et `texlod` se recalculent, hors clé.
- Liste d'instances jetée à chaque frame : clé, matrice, binding UV/header si la
  source est la RAM. La commande 02 reste un seau éphémère en espace vue, dessiné
  seulement avec la projection arcade.
- La 3×4 est **modèle-vue**. La persistance vient d'un ancrage : pour les clés ROM
  présentes dans les deux frames, `W_i = inverse(M_ancre) * M_i`, l'ancre étant la
  clé ROM de plus grand nombre de sommets présente à chaque frame. Le cache GPU
  garde positions et normales objet. L'interpolation ne porte que sur les matrices
  des clés présentes des deux côtés ; une clé qui apparaît/disparaît est un pop.
- **Deux passes sur le même cache** : passe oracle (matrice, `apply_focus`,
  `f2u>>8`, plans de clip, `x/z`, tri de `GpuRenderer::build`, texture RAM/luma/
  colorxlat) et passe native (vrai z, formule de luma arcade dans le shader).
  HUD et tilemaps restent le compositeur actuel.

## 3. Test de réfutation proposé

Trente-deux frames de course, caméra en mouvement. Pour chaque frame, compter les
sommets avant cull selon la source : ROM, RAM0, RAM1, direct.

Sur les clés ROM présentes dans les 32 frames, poser
`R_ij(t) = inverse(M_i(t)) * M_j(t)`. Une paire est rigide si, sur la fenêtre,
l'angle de la 3×3 relative reste sous 0,05° et la translation relative varie de
moins de 0,01. Prendre la plus grande clique rigide.

L'alternative est fausse si la ROM porte moins de la moitié des sommets avant
cull, ou si cette clique couvre moins de la moitié des sommets des clés ROM
toujours présentes. Dans ce cas les matrices ne factorisent pas un monde rigide,
et le cache ROM n'est pas la scène.

## 4. Statut

Hypothèse non implémentée. Prochaine étape : instrumenter `geo_object_data` et le
comptage de sommets avant cull, capturer 32 frames, exécuter le test de clique.
