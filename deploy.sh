#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

BUILD_IMAGES=true
PULL_IMAGES=false
HEALTH_TIMEOUT_SECONDS="${DISPATCHER_DEPLOY_TIMEOUT:-300}"

usage() {
  cat <<'EOF'
多机器人调度系统一键部署

用法：
  ./deploy.sh             生成配置、构建镜像、迁移数据库并启动服务
  ./deploy.sh --pull      构建前拉取最新基础镜像
  ./deploy.sh --no-build  跳过镜像构建，只迁移并启动已有镜像
  ./deploy.sh --help      显示帮助

首次部署可选环境变量：
  DISPATCHER_DB_PASSWORD  指定数据库密码；未设置时自动生成随机密码
  DISPATCHER_API_PORT     API 端口，默认 8080
  DISPATCHER_WEB_PORT     Web 端口，默认 8088
  DISPATCHER_DEPLOY_TIMEOUT  健康检查等待秒数，默认 300
EOF
}

while (($# > 0)); do
  case "$1" in
    --pull)
      PULL_IMAGES=true
      ;;
    --no-build)
      BUILD_IMAGES=false
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "未知参数：$1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

log() {
  printf '\n[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

fail() {
  echo "部署失败：$*" >&2
  exit 1
}

on_error() {
  local exit_code=$?
  echo >&2
  echo "部署过程中发生错误（退出码 ${exit_code}）。最近容器状态：" >&2
  docker compose ps >&2 2>/dev/null || true
  echo "可执行 docker compose logs --tail=200 查看详细日志。" >&2
  exit "${exit_code}"
}
trap on_error ERR

command -v docker >/dev/null 2>&1 || fail "未安装 Docker。请先安装 Docker Engine 和 Docker Compose v2。"
docker compose version >/dev/null 2>&1 || fail "未安装 Docker Compose v2（需要 docker compose 命令）。"
docker info >/dev/null 2>&1 || fail "无法连接 Docker daemon。请启动 Docker，或为当前用户配置 Docker 权限。"

generate_password() {
  if command -v openssl >/dev/null 2>&1; then
    openssl rand -hex 24
  else
    od -An -N24 -tx1 /dev/urandom | tr -d ' \n'
  fi
}

ensure_env_file() {
  if [[ -f .env ]]; then
    log "复用现有 .env 配置"
    if grep -q '^POSTGRES_PASSWORD=replace_for_site$' .env; then
      echo "警告：.env 仍使用示例数据库密码 replace_for_site，正式环境请尽快修改。" >&2
    fi
    return
  fi

  local db_password="${DISPATCHER_DB_PASSWORD:-$(generate_password)}"
  local api_port="${DISPATCHER_API_PORT:-8080}"
  local web_port="${DISPATCHER_WEB_PORT:-8088}"

  [[ "${api_port}" =~ ^[0-9]+$ ]] || fail "DISPATCHER_API_PORT 必须是数字"
  [[ "${web_port}" =~ ^[0-9]+$ ]] || fail "DISPATCHER_WEB_PORT 必须是数字"
  ((api_port >= 1 && api_port <= 65535)) || fail "API 端口必须在 1-65535 之间"
  ((web_port >= 1 && web_port <= 65535)) || fail "Web 端口必须在 1-65535 之间"
  [[ "${db_password}" =~ ^[A-Za-z0-9._-]+$ ]] ||
    fail "数据库密码只能包含字母、数字、点、下划线和连字符，以兼容数据库连接 URL"

  umask 077
  {
    printf 'POSTGRES_DB=dispatcher\n'
    printf 'POSTGRES_USER=dispatcher\n'
    printf 'POSTGRES_PASSWORD=%s\n' "${db_password}"
    printf 'API_PORT=%s\n' "${api_port}"
    printf 'WEB_PORT=%s\n' "${web_port}"
    printf 'DISPATCHER_SSH_SECRET_ROOTS=/run/secrets:/etc/dispatcher/ssh\n'
  } > .env
  chmod 600 .env
  log "已生成 .env（数据库密码为随机值，文件权限 600）"
}

compose_psql_value() {
  local sql="$1"
  docker compose exec -T postgres sh -c \
    'psql -v ON_ERROR_STOP=1 -U "$POSTGRES_USER" -d "$POSTGRES_DB" -Atc "$1"' \
    sh "${sql}" | tr -d '\r\n'
}

apply_migration() {
  local filename="$1"
  log "应用数据库迁移 ${filename}"
  docker compose exec -T postgres sh -c \
    'psql -v ON_ERROR_STOP=1 -U "$POSTGRES_USER" -d "$POSTGRES_DB" -f "$1"' \
    sh "/docker-entrypoint-initdb.d/${filename}"
}

wait_for_service() {
  local service="$1"
  local deadline=$((SECONDS + HEALTH_TIMEOUT_SECONDS))
  local container_id=""
  local status=""

  while ((SECONDS < deadline)); do
    container_id="$(docker compose ps -q "${service}" 2>/dev/null || true)"
    if [[ -n "${container_id}" ]]; then
      status="$(docker inspect --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' "${container_id}" 2>/dev/null || true)"
      case "${status}" in
        healthy|running)
          log "${service} 已就绪（${status}）"
          return 0
          ;;
        exited|dead|unhealthy)
          docker compose logs --tail=100 "${service}" >&2 || true
          fail "${service} 状态异常：${status}"
          ;;
      esac
    fi
    sleep 2
  done

  docker compose logs --tail=100 "${service}" >&2 || true
  fail "等待 ${service} 就绪超时（${HEALTH_TIMEOUT_SECONDS} 秒）"
}

backup_database() {
  mkdir -p backups
  chmod 700 backups
  local backup_path="backups/dispatcher-before-migration-$(date '+%Y%m%d-%H%M%S').sql"
  log "升级旧数据库前备份到 ${backup_path}"
  docker compose exec -T postgres sh -c \
    'pg_dump -U "$POSTGRES_USER" "$POSTGRES_DB"' > "${backup_path}"
  chmod 600 "${backup_path}"
}

upgrade_legacy_database() {
  local base_schema
  local has_startup_profiles
  local has_naviai_default
  local supports_ssh_capability
  local robot_delete_sets_null
  local nav_tol_defaults
  local needs_0015=false
  local needs_0016=false
  local needs_0017=false
  local needs_0018=false
  local needs_0019=false

  base_schema="$(compose_psql_value "SELECT COALESCE(to_regclass('dispatch.scenes')::text, '');")"
  [[ "${base_schema}" == "dispatch.scenes" ]] ||
    fail "数据库基础结构不存在；请检查 postgres 初始化日志和 db/migrations 文件。"

  has_startup_profiles="$(compose_psql_value "SELECT CASE WHEN to_regclass('dispatch.robot_startup_profiles') IS NULL THEN 'no' ELSE 'yes' END;")"
  if [[ "${has_startup_profiles}" != "yes" ]]; then
    needs_0015=true
    needs_0016=true
    needs_0017=true
    needs_0018=true
    needs_0019=true
  else
    has_naviai_default="$(compose_psql_value "SELECT CASE WHEN position('naviai' in COALESCE(column_default, '')) > 0 THEN 'yes' ELSE 'no' END FROM information_schema.columns WHERE table_schema='dispatch' AND table_name='robot_startup_profiles' AND column_name='ssh_username';")"
    [[ "${has_naviai_default}" == "yes" ]] || needs_0016=true

    supports_ssh_capability="$(compose_psql_value "SELECT CASE WHEN EXISTS (SELECT 1 FROM pg_constraint WHERE conrelid='dispatch.capability_definitions'::regclass AND conname='capability_definitions_operation_kind_check' AND position('SSH' in pg_get_constraintdef(oid)) > 0) THEN 'yes' ELSE 'no' END;")"
    [[ "${supports_ssh_capability}" == "yes" ]] || needs_0017=true

    robot_delete_sets_null="$(compose_psql_value "SELECT CASE WHEN EXISTS (SELECT 1 FROM pg_constraint WHERE conrelid='dispatch.node_runs'::regclass AND conname='node_runs_assigned_robot_id_fkey' AND position('ON DELETE SET NULL' in pg_get_constraintdef(oid)) > 0) THEN 'yes' ELSE 'no' END;")"
    [[ "${robot_delete_sets_null}" == "yes" ]] || needs_0018=true

    nav_tol_defaults="$(compose_psql_value "SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM dispatch.capability_definitions WHERE capability_key = 'navigation') THEN 'yes' WHEN EXISTS (SELECT 1 FROM dispatch.capability_definitions WHERE capability_key = 'navigation' AND (COALESCE((parameter_schema#>>'{properties,distance_tolerance,default}')::numeric, -1) IS DISTINCT FROM 0.04 OR COALESCE((parameter_schema#>>'{properties,heading_tolerance,default}')::numeric, -1) IS DISTINCT FROM 0.04 OR COALESCE((protocol_config->>'default_distance_tolerance')::numeric, -1) IS DISTINCT FROM 0.04 OR COALESCE((protocol_config->>'default_heading_tolerance')::numeric, -1) IS DISTINCT FROM 0.04)) THEN 'no' ELSE 'yes' END;")"
    [[ "${nav_tol_defaults}" == "yes" ]] || needs_0019=true
  fi

  if [[ "${needs_0015}" == false && "${needs_0016}" == false && "${needs_0017}" == false && "${needs_0018}" == false && "${needs_0019}" == false ]]; then
    log "数据库结构已是当前版本"
    return
  fi

  backup_database
  [[ "${needs_0015}" == true ]] && apply_migration "0015_linked_runs_and_startup_profiles.sql"
  [[ "${needs_0016}" == true ]] && apply_migration "0016_robot_ssh_profiles.sql"
  [[ "${needs_0017}" == true ]] && apply_migration "0017_ssh_capability_kind.sql"
  [[ "${needs_0018}" == true ]] && apply_migration "0018_robot_delete_run_history.sql"
  [[ "${needs_0019}" == true ]] && apply_migration "0019_navigation_tolerance_defaults.sql"
}

ensure_env_file

if [[ "${BUILD_IMAGES}" == true ]]; then
  log "构建 dispatcher 和 web 镜像"
  build_args=()
  [[ "${PULL_IMAGES}" == true ]] && build_args+=(--pull)
  docker compose build "${build_args[@]}" dispatcher web
else
  log "已跳过镜像构建"
fi

log "启动 PostgreSQL"
docker compose up -d postgres
wait_for_service postgres
upgrade_legacy_database

log "启动 dispatcher 和 web"
docker compose up -d dispatcher web
wait_for_service dispatcher
wait_for_service web

log "部署完成"
docker compose ps

web_address="$(docker compose port web 80 2>/dev/null || true)"
api_address="$(docker compose port dispatcher 8080 2>/dev/null || true)"
printf '\nWeb： http://%s\n' "${web_address:-127.0.0.1:8088}"
printf 'API： http://%s/api/v1/health\n' "${api_address:-127.0.0.1:8080}"
printf '\n局域网访问时，请把 0.0.0.0 替换为部署服务器 IP。\n'
printf '日志：docker compose logs -f dispatcher\n'
