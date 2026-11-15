#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

REMOTE="https://github.com/ElxSnowkk/PlasmaDroid.git"

if [ ! -d .git ]; then
  git init -q -b main
fi

if ! git remote get-url origin >/dev/null 2>&1; then
  git remote add origin "$REMOTE"
fi

read -r -p "Nome do commit: " MSG
if [ -z "${MSG//[[:space:]]/}" ]; then
  echo "O nome do commit não pode ficar vazio."
  exit 1
fi

git add -A
if git diff --cached --quiet; then
  echo "Nada novo para commitar."
else
  git commit -q -m "$MSG"
fi

BRANCH="$(git symbolic-ref --short HEAD)"
git push -u origin "$BRANCH"
echo "Enviado para $REMOTE ($BRANCH)."
