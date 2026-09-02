#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

usage() {
  cat <<'EOF'
把运行所需 Docker 镜像打成 tar，拷到离线机器后加载，避免再从网络拉镜像或现场编译。

用法：
  ./scripts/pack_images.sh export [文件]
  ./scripts/pack_images.sh import [文件]

不指定文件时：
  export 写到 backups/dispatcher-images-时间戳.tar.gz
  import 使用 backups/ 里最新的一份

本机需已构建过镜像（先 ./deploy.sh）。对面：
  ./scripts/pack_images.sh import
  ./deploy.sh --no-build
EOF
}

log() {
  printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

fail() {
  echo "失败：$*" >&2
  exit 1
}

command -v docker >/dev/null 2>&1 || fail "未安装 Docker"
docker compose version >/dev/null 2>&1 || fail "需要 docker compose"
[[ -f docker-compose.yml ]] || fail "请在仓库根目录执行（找不到 docker-compose.yml）。"

latest_backup() {
  shopt -s nullglob
  local files=(backups/dispatcher-images-*.tar.gz)
  shopt -u nullglob
  ((${#files[@]} > 0)) || fail "backups/ 下没有 dispatcher-images-*.tar.gz，请先 export 或指定文件"
  ls -1t "${files[@]}" | head -n 1
}

compose_project_name() {
  local name=""
  if command -v python3 >/dev/null 2>&1; then
    name="$(docker compose config --format json 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["name"])' 2>/dev/null || true)"
  fi
  if [[ -z "${name}" ]]; then
    name="dispatcher"
  fi
  printf '%s\n' "${name}"
}

runtime_images() {
  local project
  project="$(compose_project_name)"
  printf '%s\n' \
    "postgres:16-alpine" \
    "${project}-dispatcher:latest" \
    "${project}-web:latest"
}

image_exists() {
  docker image inspect "$1" >/dev/null 2>&1
}

export_images() {
  mkdir -p backups
  chmod 700 backups
  local dest="${1:-}"
  if [[ -z "${dest}" ]]; then
    dest="backups/dispatcher-images-$(date '+%Y%m%d-%H%M%S').tar.gz"
  fi
  mkdir -p "$(dirname -- "${dest}")"

  local images=()
  local missing=()
  local name
  while IFS= read -r name; do
    if image_exists "${name}"; then
      images+=("${name}")
    else
      missing+=("${name}")
    fi
  done < <(runtime_images)

  if ((${#missing[@]} > 0)); then
    fail "本地缺少镜像：${missing[*]}。请先在本机 ./deploy.sh 构建后再导出。"
  fi

  log "保存镜像：${images[*]}"
  log "写入 ${dest}（可能需要几分钟）"
  docker save "${images[@]}" | gzip -1 > "${dest}"
  chmod 600 "${dest}"
  log "导出完成：${dest}（$(du -h "${dest}" | awk '{print $1}')）"
}

import_images() {
  local src="${1:-}"
  if [[ -z "${src}" ]]; then
    src="$(latest_backup)"
  fi
  [[ -f "${src}" ]] || fail "找不到文件 ${src}"
  log "加载镜像：${src}"
  docker load -i "${src}"
  log "加载完成。离线启动请执行：./deploy.sh --no-build"
}

ACTION="${1:-}"
shift || true

case "${ACTION}" in
  export)
    export_images "${1:-}"
    ;;
  import)
    import_images "${1:-}"
    ;;
  -h|--help|"")
    usage
    ;;
  *)
    usage >&2
    fail "未知命令：${ACTION}"
    ;;
esac
