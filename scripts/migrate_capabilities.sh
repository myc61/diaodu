#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

usage() {
  cat <<'EOF'
一键导出 / 导入能力模板（capability_profiles + capability_definitions）

用法：
  ./scripts/migrate_capabilities.sh export [文件]
  ./scripts/migrate_capabilities.sh import [文件]

不指定文件时：
  export 写到 backups/capability-templates-时间戳.json
  import 使用 backups/ 里最新的一份

对面机器先 ./deploy.sh，再拷贝该 JSON 后执行 import。
同名能力（profile_id + capability_key）会覆盖更新，新能力会插入。
不包含流程、点位、地图和机器人。完整工作区请用 ./scripts/migrate_workspace.sh。
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

postgres_ready() {
  local container_id
  container_id="$(docker compose ps -q postgres 2>/dev/null || true)"
  [[ -n "${container_id}" ]] || fail "postgres 未启动。请先 ./deploy.sh 或 docker compose up -d postgres"
  docker compose exec -T postgres sh -c \
    'pg_isready -U "$POSTGRES_USER" -d "$POSTGRES_DB"' >/dev/null 2>&1 ||
    fail "postgres 尚未就绪"
}

compose_psql() {
  docker compose exec -T postgres sh -c \
    'psql -v ON_ERROR_STOP=1 -U "$POSTGRES_USER" -d "$POSTGRES_DB"'
}

validate_pack() {
  local path="$1"
  if command -v python3 >/dev/null 2>&1; then
    python3 - "${path}" <<'PY'
import json, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as handle:
    doc = json.load(handle)
if doc.get("format") != "diaodu.capability_templates":
    raise SystemExit("unexpected format")
print(
    f"profiles={len(doc.get('profiles') or [])} "
    f"capabilities={len(doc.get('capabilities') or [])}"
)
PY
  elif grep -q 'diaodu.capability_templates' "${path}"; then
    log "已检查到能力模板包（未安装 python3，跳过计数）"
  else
    fail "文件不是能力模板导出包"
  fi
}

latest_backup() {
  local latest=""
  shopt -s nullglob
  local files=(backups/capability-templates-*.json)
  shopt -u nullglob
  ((${#files[@]} > 0)) || fail "backups/ 下没有 capability-templates-*.json，请先 export 或指定文件"
  latest="$(ls -1t "${files[@]}" | head -n 1)"
  printf '%s\n' "${latest}"
}

export_templates() {
  postgres_ready
  mkdir -p backups
  chmod 700 backups
  local dest="${1:-}"
  if [[ -z "${dest}" ]]; then
    dest="backups/capability-templates-$(date '+%Y%m%d-%H%M%S').json"
  fi
  mkdir -p "$(dirname -- "${dest}")"

  log "导出能力模板到 ${dest}"
  docker compose exec -T postgres sh -c \
    'psql -v ON_ERROR_STOP=1 -U "$POSTGRES_USER" -d "$POSTGRES_DB" -Atc "
      SELECT jsonb_build_object(
        '\''format'\'', '\''diaodu.capability_templates'\'',
        '\''version'\'', 1,
        '\''exported_at'\'', now(),
        '\''profiles'\'', (
          SELECT COALESCE(jsonb_agg(to_jsonb(p) ORDER BY p.name), '\''[]'\''::jsonb)
          FROM dispatch.capability_profiles p
        ),
        '\''capabilities'\'', (
          SELECT COALESCE(jsonb_agg(to_jsonb(c) ORDER BY c.capability_key), '\''[]'\''::jsonb)
          FROM dispatch.capability_definitions c
        )
      );"' > "${dest}"

  [[ -s "${dest}" ]] || fail "导出结果为空"
  validate_pack "${dest}"
  chmod 600 "${dest}"
  log "导出完成：${dest}"
}

import_templates() {
  postgres_ready
  local src="${1:-}"
  if [[ -z "${src}" ]]; then
    src="$(latest_backup)"
  fi
  [[ -f "${src}" ]] || fail "找不到文件 ${src}"

  validate_pack "${src}" || fail "导入文件格式不对，需要 migrate_capabilities.sh export 生成的 JSON"

  log "导入能力模板：${src}"
  {
    cat <<'SQL'
BEGIN;
CREATE TEMP TABLE _capability_pack (doc jsonb);
COPY _capability_pack FROM STDIN;
SQL
    cat "${src}"
    printf '\n'
    cat <<'SQL'
\.

DO $$
DECLARE
  pack jsonb;
BEGIN
  SELECT doc INTO pack FROM _capability_pack LIMIT 1;
  IF pack IS NULL OR pack->>'format' IS DISTINCT FROM 'diaodu.capability_templates' THEN
    RAISE EXCEPTION 'invalid capability template pack';
  END IF;
END $$;

INSERT INTO dispatch.capability_profiles (id, name, description, created_at, updated_at)
SELECT
  incoming.id,
  incoming.name,
  COALESCE(incoming.description, ''),
  COALESCE(incoming.created_at, now()),
  COALESCE(incoming.updated_at, now())
FROM jsonb_to_recordset((SELECT doc->'profiles' FROM _capability_pack LIMIT 1))
  AS incoming(
    id uuid,
    name text,
    description text,
    created_at timestamptz,
    updated_at timestamptz
  )
ON CONFLICT (id) DO UPDATE SET
  name = EXCLUDED.name,
  description = EXCLUDED.description,
  updated_at = now();

CREATE TEMP TABLE _incoming_capabilities AS
SELECT *
FROM jsonb_to_recordset((SELECT doc->'capabilities' FROM _capability_pack LIMIT 1))
  AS incoming(
    id uuid,
    profile_id uuid,
    capability_key text,
    operation_kind text,
    endpoint_name text,
    ros_message_type text,
    parameter_schema jsonb,
    request_template jsonb,
    feedback_mapping jsonb,
    result_mapping jsonb,
    success_condition jsonb,
    failure_condition jsonb,
    timeout_ms integer,
    retry_policy jsonb,
    cancel_policy jsonb,
    blocking_type text,
    resource_claims jsonb,
    created_at timestamptz,
    updated_at timestamptz,
    protocol_config jsonb,
    motion_ownership text,
    event_specs jsonb
  );

UPDATE dispatch.capability_definitions AS dest
SET
  operation_kind = src.operation_kind,
  endpoint_name = src.endpoint_name,
  ros_message_type = src.ros_message_type,
  parameter_schema = COALESCE(src.parameter_schema, '{}'::jsonb),
  request_template = COALESCE(src.request_template, '{}'::jsonb),
  feedback_mapping = COALESCE(src.feedback_mapping, '{}'::jsonb),
  result_mapping = COALESCE(src.result_mapping, '{}'::jsonb),
  success_condition = src.success_condition,
  failure_condition = src.failure_condition,
  timeout_ms = COALESCE(src.timeout_ms, dest.timeout_ms),
  retry_policy = COALESCE(src.retry_policy, '{}'::jsonb),
  cancel_policy = COALESCE(src.cancel_policy, '{}'::jsonb),
  blocking_type = COALESCE(src.blocking_type, dest.blocking_type),
  resource_claims = COALESCE(src.resource_claims, '[]'::jsonb),
  protocol_config = COALESCE(src.protocol_config, '{}'::jsonb),
  motion_ownership = COALESCE(src.motion_ownership, dest.motion_ownership),
  event_specs = COALESCE(src.event_specs, '[]'::jsonb),
  updated_at = now()
FROM _incoming_capabilities AS src
WHERE dest.profile_id = src.profile_id
  AND dest.capability_key = src.capability_key;

INSERT INTO dispatch.capability_definitions (
  id,
  profile_id,
  capability_key,
  operation_kind,
  endpoint_name,
  ros_message_type,
  parameter_schema,
  request_template,
  feedback_mapping,
  result_mapping,
  success_condition,
  failure_condition,
  timeout_ms,
  retry_policy,
  cancel_policy,
  blocking_type,
  resource_claims,
  created_at,
  updated_at,
  protocol_config,
  motion_ownership,
  event_specs
)
SELECT
  src.id,
  src.profile_id,
  src.capability_key,
  src.operation_kind,
  src.endpoint_name,
  src.ros_message_type,
  COALESCE(src.parameter_schema, '{}'::jsonb),
  COALESCE(src.request_template, '{}'::jsonb),
  COALESCE(src.feedback_mapping, '{}'::jsonb),
  COALESCE(src.result_mapping, '{}'::jsonb),
  src.success_condition,
  src.failure_condition,
  COALESCE(src.timeout_ms, 30000),
  COALESCE(src.retry_policy, '{}'::jsonb),
  COALESCE(src.cancel_policy, '{}'::jsonb),
  COALESCE(src.blocking_type, 'NONE'),
  COALESCE(src.resource_claims, '[]'::jsonb),
  COALESCE(src.created_at, now()),
  COALESCE(src.updated_at, now()),
  COALESCE(src.protocol_config, '{}'::jsonb),
  COALESCE(src.motion_ownership, 'DISPATCHER'),
  COALESCE(src.event_specs, '[]'::jsonb)
FROM _incoming_capabilities AS src
WHERE NOT EXISTS (
  SELECT 1
  FROM dispatch.capability_definitions AS dest
  WHERE dest.profile_id = src.profile_id
    AND dest.capability_key = src.capability_key
);

SELECT
  (SELECT count(*) FROM dispatch.capability_profiles) AS profiles,
  (SELECT count(*) FROM dispatch.capability_definitions) AS capabilities;

COMMIT;
SQL
  } | compose_psql >/dev/null

  log "导入完成。刷新能力模板页即可看到结果。"
}

ACTION="${1:-}"
shift || true

case "${ACTION}" in
  export)
    export_templates "${1:-}"
    ;;
  import)
    import_templates "${1:-}"
    ;;
  -h|--help|"")
    usage
    ;;
  *)
    usage >&2
    fail "未知命令：${ACTION}"
    ;;
esac
