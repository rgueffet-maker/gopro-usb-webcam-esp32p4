# Commandes Git utiles pour le projet

Ce fichier résume les commandes Git les plus importantes pour créer un projet, lier un projet à GitHub, puis enregistrer les modifications.

---

## 1. Vérifier l'état du projet

Avant d'enregistrer des modifications, on peut vérifier les fichiers modifiés avec :

```bash
git status
```

Cette commande permet de voir :

```text
- les fichiers modifiés ;
- les nouveaux fichiers ;
- les fichiers déjà prêts à être enregistrés.
```

---

## 2. Enregistrer les modifications d'un projet déjà lié à GitHub

Quand le projet existe déjà sur GitHub, la procédure normale est :

```bash
git status
git add .
git commit -m "Message clair sur les modifications"
git push
```

Exemple :

```bash
git add .
git commit -m "Ajout version stable GoPro USB ESP32-P4"
git push
```

Explication :

```text
git add .      -> prépare tous les fichiers modifiés
git commit     -> enregistre une nouvelle version locale
git push       -> envoie cette version sur GitHub
```

---

## 3. Créer un nouveau projet Git depuis zéro

Dans le dossier du projet :

```bash
git init
git add .
git commit -m "Initial commit"
```

Ensuite, créer un nouveau repository vide sur GitHub.

Puis lier le dossier local au repository GitHub :

```bash
git branch -M main
git remote add origin https://github.com/TON_NOM/TON_REPO.git
git push -u origin main
```

Exemple :

```bash
git remote add origin https://github.com/rgueffet/GOPRO_P4_USB.git
git push -u origin main
```

---

## 4. Vérifier si le projet est déjà lié à GitHub

Dans le dossier du projet :

```bash
git remote -v
```

Si le projet est déjà lié, on voit quelque chose comme :

```text
origin  https://github.com/rgueffet/nom-du-projet.git (fetch)
origin  https://github.com/rgueffet/nom-du-projet.git (push)
```

Si rien ne s'affiche, il faut ajouter le lien GitHub :

```bash
git remote add origin https://github.com/TON_NOM/TON_REPO.git
```

---

## 5. Modifier un projet déjà créé

À chaque fois que le code ou le README est modifié :

```bash
git status
git add .
git commit -m "Description de la modification"
git push
```

Exemples de messages de commit :

```bash
git commit -m "Correction recovery USB GoPro après reboot caméra"
```

```bash
git commit -m "Ajout README français pour ESP32-P4 GoPro USB"
```

```bash
git commit -m "Stabilisation du redémarrage live après reconnexion USB"
```

---

## 6. Commandes à retenir

### Projet déjà existant

```bash
git status
git add .
git commit -m "Message"
git push
```

### Nouveau projet

```bash
git init
git add .
git commit -m "Initial commit"
git branch -M main
git remote add origin https://github.com/TON_NOM/TON_REPO.git
git push -u origin main
```

---

## 7. Cas concret pour ce projet

Pour enregistrer la version stable du projet GoPro USB ESP32-P4 :

```bash
git status
git add .
git commit -m "Ajout version stable GoPro USB ESP32-P4"
git push
```

Si le README a été modifié :

```bash
git add README.md
git commit -m "Mise à jour README GoPro USB ESP32-P4"
git push
```

Si le fichier principal a été modifié :

```bash
git add main/eth_hello_main.c
git commit -m "Correction recovery USB GoPro ESP32-P4"
git push
```
