#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

TABLES=(
  capability_profiles
  capability_definitions
  scenes
  map_versions
  robots
  robot_connections
  robot_startup_profiles
  stations
  station_actions
  scene_robots
  workflow_definitions
  workflow_versions
)

usage() {
  cat <<'EOF'
一键导出 / 导入工作区：能力模板、机器人、场景、地图文件、点位、点位动作、流程。

用法：
  ./scripts/migrate_workspace.sh export [文件.tar.gz]
  ./scripts/migrate_workspace.sh import [文件.tar.gz]

不指定文件时：
  export 写到 backups/workspace-pack-时间戳.tar.gz
  import 使用 backups/ 里最新的一份

对面先 ./deploy.sh，再拷贝该压缩包后执行 import。
导入会替换目标库中的上述业务数据（含因此级联删除的运行历史），
不包含 SSH 私钥。导入后请在机器人页核对 IP / rosbridge。
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

latest_backup() {
  shopt -s nullglob
  local files=(backups/workspace-pack-*.tar.gz)
  shopt -u nullglob
  ((${#files[@]} > 0)) || fail "backups/ 下没有 workspace-pack-*.tar.gz，请先 export 或指定文件"
  ls -1t "${files[@]}" | head -n 1
}

resolve_map_volume() {
  local cid name
  cid="$(docker compose ps -q dispatcher 2>/dev/null || true)"
  if [[ -n "${cid}" ]]; then
    name="$(docker inspect -f '{{ range .Mounts }}{{ if eq .Destination "/var/lib/dispatcher/maps" }}{{ .Name }}{{ end }}{{ end }}' "${cid}")"
    if [[ -n "${name}" ]]; then
      printf '%s\n' "${name}"
      return
    fi
  fi
  if docker volume inspect dispatcher_map-data >/dev/null 2>&1; then
    printf '%s\n' dispatcher_map-data
    return
  fi
  name="$(docker volume ls -q | grep -E '_map-data$' | head -n 1 || true)"
  [[ -n "${name}" ]] || fail "找不到 map-data 卷。请先启动过一次 dispatcher。"
  printf '%s\n' "${name}"
}

export_maps() {
  local dest="$1"
  local cid volume
  cid="$(docker compose ps -q dispatcher 2>/dev/null || true)"
  if [[ -n "${cid}" ]]; then
    docker compose exec -T dispatcher tar -C /var/lib/dispatcher/maps -cf - . > "${dest}"
    return
  fi
  volume="$(resolve_map_volume)"
  docker run --rm -v "${volume}:/maps:ro" alpine:3.20 tar -C /maps -cf - . > "${dest}"
}

import_maps() {
  local src="$1"
  local cid volume
  [[ -s "${src}" ]] || {
    log "压缩包里没有地图文件，跳过地图卷"
    return
  }
  cid="$(docker compose ps -q dispatcher 2>/dev/null || true)"
  if [[ -z "${cid}" ]]; then
    log "启动 dispatcher 以便写入地图卷"
    docker compose up -d dispatcher
    cid="$(docker compose ps -q dispatcher)"
  fi
  if [[ -n "${cid}" ]]; then
    docker compose exec -T -u root dispatcher tar -C /var/lib/dispatcher/maps -xf - < "${src}"
    docker compose exec -T -u root dispatcher \
      chown -R dispatcher:dispatcher /var/lib/dispatcher/maps
    return
  fi
  volume="$(resolve_map_volume)"
  docker run --rm -i -v "${volume}:/maps" alpine:3.20 tar -C /maps -xf - < "${src}"
}

validate_pack_dir() {
  local dir="$1"
  [[ -f "${dir}/manifest.json" && -f "${dir}/data.json" ]] ||
    fail "压缩包缺少 manifest.json 或 data.json"
  if command -v python3 >/dev/null 2>&1; then
    python3 - "${dir}/manifest.json" "${dir}/data.json" <<'PY'
import json, sys
manifest = json.load(open(sys.argv[1], encoding="utf-8"))
data = json.load(open(sys.argv[2], encoding="utf-8"))
if manifest.get("format") != "diaodu.workspace_pack":
    raise SystemExit("bad manifest")
if data.get("format") != "diaodu.workspace_pack":
    raise SystemExit("bad data")
tables = data.get("tables") or {}
counts = " ".join(f"{name}={len(tables.get(name) or [])}" for name in (
    "scenes", "map_versions", "stations", "station_actions",
    "workflow_definitions", "capability_definitions", "robots"
))
print(counts)
PY
  elif grep -q 'diaodu.workspace_pack' "${dir}/manifest.json"; then
    log "已检查到工作区包"
  else
    fail "文件不是工作区导出包"
  fi
}

export_workspace() {
  postgres_ready
  mkdir -p backups
  chmod 700 backups
  local dest="${1:-}"
  if [[ -z "${dest}" ]]; then
    dest="backups/workspace-pack-$(date '+%Y%m%d-%H%M%S').tar.gz"
  fi
  mkdir -p "$(dirname -- "${dest}")"
  local tmp
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/diaodu-workspace-export.XXXXXX")"
  trap 'rm -rf "${tmp}"' RETURN

  log "导出数据库工作区表"
  {
    cat <<'SQL'
CREATE FUNCTION pg_temp.dump_table(tbl text) RETURNS jsonb AS $$
DECLARE
  result jsonb;
BEGIN
  IF to_regclass('dispatch.' || tbl) IS NULL THEN
    RETURN '[]'::jsonb;
  END IF;
  EXECUTE format(
    'SELECT COALESCE(jsonb_agg(to_jsonb(t)), ''[]''::jsonb) FROM dispatch.%I t',
    tbl
  ) INTO result;
  RETURN result;
END;
$$ LANGUAGE plpgsql;

SELECT jsonb_build_object(
  'format', 'diaodu.workspace_pack',
  'version', 1,
  'exported_at', now(),
  'tables', jsonb_build_object(
    'capability_profiles', pg_temp.dump_table('capability_profiles'),
    'capability_definitions', pg_temp.dump_table('capability_definitions'),
    'scenes', pg_temp.dump_table('scenes'),
    'map_versions', pg_temp.dump_table('map_versions'),
    'robots', pg_temp.dump_table('robots'),
    'robot_connections', pg_temp.dump_table('robot_connections'),
    'robot_startup_profiles', pg_temp.dump_table('robot_startup_profiles'),
    'stations', pg_temp.dump_table('stations'),
    'station_actions', pg_temp.dump_table('station_actions'),
    'scene_robots', pg_temp.dump_table('scene_robots'),
    'workflow_definitions', pg_temp.dump_table('workflow_definitions'),
    'workflow_versions', pg_temp.dump_table('workflow_versions')
  )
);
SQL
  } | docker compose exec -T postgres sh -c \
    'psql -v ON_ERROR_STOP=1 -U "$POSTGRES_USER" -d "$POSTGRES_DB" -At' \
    | awk '/^{/' > "${tmp}/data.json"

  [[ -s "${tmp}/data.json" ]] || fail "数据库导出为空"
  printf '%s\n' '{"format":"diaodu.workspace_pack","version":1,"includes_maps":true}' \
    > "${tmp}/manifest.json"

  log "导出地图卷"
  export_maps "${tmp}/maps.tar"

  tar -C "${tmp}" -czf "${dest}" manifest.json data.json maps.tar
  chmod 600 "${dest}"
  validate_pack_dir "${tmp}"
  log "导出完成：${dest}"
}

import_workspace() {
  postgres_ready
  local src="${1:-}"
  if [[ -z "${src}" ]]; then
    src="$(latest_backup)"
  fi
  [[ -f "${src}" ]] || fail "找不到文件 ${src}"

  local tmp
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/diaodu-workspace-import.XXXXXX")"
  trap 'rm -rf "${tmp}"' RETURN
  tar -C "${tmp}" --no-same-owner -xzf "${src}" 2>/dev/null ||
    tar -C "${tmp}" -xzf "${src}" ||
    [[ -f "${tmp}/data.json" ]] ||
    fail "无法解开工作区压缩包 ${src}"
  validate_pack_dir "${tmp}"

  log "导入数据库（将替换目标工作区数据）"
  {
    cat <<'SQL'
BEGIN;
SET session_replication_role = replica;
CREATE TEMP TABLE _workspace_pack (doc jsonb);
SQL
    if command -v python3 >/dev/null 2>&1; then
      python3 - "${tmp}/data.json" <<'PY'
import json, sys
raw = json.dumps(
    json.load(open(sys.argv[1], encoding="utf-8")),
    ensure_ascii=False,
    separators=(",", ":"),
)
tag = "wsdata"
while f"${tag}$" in raw:
    tag += "x"
print(f"INSERT INTO _workspace_pack(doc) VALUES (${tag}${raw}${tag}$::jsonb);")
PY
    else
      fail "导入需要 python3，用于把 data.json 写成一条 SQL"
    fi
    cat <<'SQL'
DO $$
DECLARE
  pack jsonb;
  names text[] := ARRAY[
    'workflow_versions',
    'workflow_definitions',
    'station_actions',
    'stations',
    'scene_robots',
    'robot_startup_profiles',
    'robot_connections',
    'map_versions',
    'scenes',
    'robots',
    'capability_definitions',
    'capability_profiles'
  ];
  existing text[] := ARRAY[]::text[];
  tbl text;
  rows jsonb;
BEGIN
  SELECT doc INTO pack FROM _workspace_pack LIMIT 1;
  IF pack IS NULL OR pack->>'format' IS DISTINCT FROM 'diaodu.workspace_pack' THEN
    RAISE EXCEPTION 'invalid workspace pack';
  END IF;

  FOREACH tbl IN ARRAY names LOOP
    IF to_regclass('dispatch.' || tbl) IS NOT NULL THEN
      existing := existing || format('dispatch.%I', tbl);
    END IF;
  END LOOP;
  IF coalesce(array_length(existing, 1), 0) > 0 THEN
    EXECUTE 'TRUNCATE TABLE ' || array_to_string(existing, ', ') ||
            ' RESTART IDENTITY CASCADE';
  END IF;

  FOREACH tbl IN ARRAY ARRAY[
    'capability_profiles',
    'capability_definitions',
    'scenes',
    'map_versions',
    'robots',
    'robot_connections',
    'robot_startup_profiles',
    'stations',
    'station_actions',
    'scene_robots',
    'workflow_definitions',
    'workflow_versions'
  ] LOOP
    IF to_regclass('dispatch.' || tbl) IS NULL THEN
      CONTINUE;
    END IF;
    rows := pack->'tables'->tbl;
    IF rows IS NULL OR jsonb_typeof(rows) <> 'array' OR jsonb_array_length(rows) = 0 THEN
      CONTINUE;
    END IF;
    EXECUTE format(
      'INSERT INTO dispatch.%I SELECT * FROM jsonb_populate_recordset(NULL::dispatch.%I, $1)',
      tbl, tbl
    ) USING rows;
  END LOOP;

  IF to_regclass('dispatch.robot_connections') IS NOT NULL THEN
    UPDATE dispatch.robot_connections
    SET state = 'DISCONNECTED',
        connected_at = NULL;
  END IF;
END $$;

SET session_replication_role = origin;
COMMIT;
SQL
  } | compose_psql

  log "导入地图文件到 Docker 卷"
  import_maps "${tmp}/maps.tar"
  if docker compose ps -q dispatcher >/dev/null 2>&1; then
    local previews
    previews="$(docker compose exec -T dispatcher \
      sh -c 'find /var/lib/dispatcher/maps -name preview.png -type f | wc -l' \
      2>/dev/null || printf '0')"
    log "地图卷中的 preview.png 数量：${previews}"
    if [[ "${previews// /}" == "0" ]]; then
      log "警告：没有预览图。场景页会只有点位没有底图。请确认导入包含 maps.tar，或重新导入。"
    fi
  fi
  log "导入完成。刷新页面后应能看到场景、点位和流程。"
  log "机器人连接状态已重置，请确认 IP / rosbridge 后等待重连。"
}

# keep TABLES referenced so the whitelist stays visible in the script
: "${TABLES[@]}"

ACTION="${1:-}"
shift || true

case "${ACTION}" in
  export)
    export_workspace "${1:-}"
    ;;
  import)
    import_workspace "${1:-}"
    ;;
  -h|--help|"")
    usage
    ;;
  *)
    usage >&2
    fail "未知命令：${ACTION}"
    ;;
esac
