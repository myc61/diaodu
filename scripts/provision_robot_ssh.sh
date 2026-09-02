#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

HOST=""
SSH_USER="naviai"
SSH_PORT=22
CONFIG_HOME="${XDG_CONFIG_HOME:-${HOME:-/tmp}/.config}"
SSH_ROOT="${DISPATCHER_SSH_ROOT:-${CONFIG_HOME}/dispatcher/ssh}"
COMPOSE_OVERRIDE="${PROJECT_ROOT}/docker-compose.override.yml"
CONTAINER_KEY="/etc/dispatcher/ssh/robot_ssh_key"
CONTAINER_KNOWN_HOSTS="/etc/dispatcher/ssh/robot_known_hosts"
PROFILE_NAME="robot_bringup"
REMOTE_SCRIPT="./start.sh"
WORKING_DIRECTORY="/home/naviai/robot"
LOG_PATH="/tmp/dispatcher-robot-start.log"
READINESS_SCRIPT=""
STEP_MODE="detached"
TOTAL_TIMEOUT_MS=120000
STEP_TIMEOUT_MS=60000
READINESS_RETRY_COUNT=30
READINESS_RETRY_INTERVAL_MS=2000
API_URL="http://127.0.0.1:8080"
ROBOT_ID=""
REGISTER_PROFILE=false
APPLY_COMPOSE=true
ACCEPT_HOST_KEY=false

usage() {
  cat <<'EOF'
机器人 SSH 一键初始化

用法：
  ./scripts/provision_robot_ssh.sh --host 192.168.1.20

脚本会：
  1. 生成或复用 Dispatcher SSH 密钥；
  2. 记录机器人 known_hosts（首次运行会确认主机指纹）；
  3. 使用一次机器人密码执行 ssh-copy-id；
  4. 生成 docker-compose.override.yml，挂载 dispatcher 专用 SSH 卷；
  5. 把私钥和 known_hosts 拷入容器并改为 dispatcher 用户可读；
  6. 生成可粘贴到页面的启动方案 JSON；
  7. 默认重启 dispatcher 并从容器内测试 SSH。

常用参数：
  --host HOST                  机器人 IP 或主机名（必填）
  --user USER                  SSH 用户，默认 naviai
  --port PORT                  SSH 端口，默认 22
  --profile-name NAME          启动方案名称，默认 robot_bringup
  --remote-script PATH         机器人端启动脚本，默认 ./start.sh
  --working-directory PATH     机器人端工作目录，默认 /home/naviai/robot
  --log-path PATH              detached 模式的机器人端日志，默认 /tmp/dispatcher-robot-start.log
  --readiness-script PATH      可选的机器人端就绪检查脚本
  --mode MODE                  wait 或 detached，默认 detached
  --timeout-ms MS              启动方案总超时，默认 120000
  --step-timeout-ms MS         启动步骤超时，默认 60000
  --robot-id UUID              配合 --register-profile 自动创建启动方案
  --api-url URL                API 地址，默认 http://127.0.0.1:8080
  --register-profile           通过 API 自动创建启动方案
  --ssh-root DIR               宿主机密钥目录，默认 ~/.config/dispatcher/ssh
  --compose-override FILE      Compose 覆盖文件，默认 ./docker-compose.override.yml
  --no-compose                 只准备文件，不重启或测试 Docker
  --yes                        跳过主机指纹确认（不推荐）
  -h, --help                   显示帮助

首次运行仍需要输入一次机器人 SSH 密码；之后使用密钥登录，不再需要密码。
EOF
}

fail() {
  printf '错误：%s\n' "$*" >&2
  exit 1
}

log() {
  printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
}

require_command() {
  command -v "$1" >/dev/null 2>&1 ||
    fail "缺少命令 $1，请先安装后重试"
}

require_value() {
  (($# >= 2)) || fail "参数 $1 缺少值"
  [[ -n "$2" ]] || fail "参数 $1 的值不能为空"
}

is_integer() {
  [[ "$1" =~ ^[0-9]+$ ]]
}

validate_remote_value() {
  local value="$1"
  [[ -n "$value" ]] || fail "远程路径不能为空"
  [[ "$value" != *$'\n'* && "$value" != *$'\r'* ]] ||
    fail "远程路径不能包含换行"
  case "$value" in
    *';'*|*'|'*|*'&'*|*'>'*|*'<'*|*'$'*|*'`'*)
      fail "远程路径不能包含 Shell 控制字符：$value"
      ;;
  esac
  case "/${value}/" in
    */../*) fail "远程路径不能包含路径穿越：$value" ;;
  esac
}

json_escape() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  value="${value//$'\n'/\\n}"
  value="${value//$'\r'/\\r}"
  value="${value//$'\t'/\\t}"
  printf '%s' "$value"
}

while (($# > 0)); do
  case "$1" in
    --host)
      require_value "$1" "${2:-}"
      HOST="$2"
      shift
      ;;
    --user)
      require_value "$1" "${2:-}"
      SSH_USER="$2"
      shift
      ;;
    --port)
      require_value "$1" "${2:-}"
      SSH_PORT="$2"
      shift
      ;;
    --profile-name)
      require_value "$1" "${2:-}"
      PROFILE_NAME="$2"
      shift
      ;;
    --remote-script)
      require_value "$1" "${2:-}"
      REMOTE_SCRIPT="$2"
      shift
      ;;
    --working-directory)
      require_value "$1" "${2:-}"
      WORKING_DIRECTORY="$2"
      shift
      ;;
    --log-path)
      require_value "$1" "${2:-}"
      LOG_PATH="$2"
      shift
      ;;
    --readiness-script)
      require_value "$1" "${2:-}"
      READINESS_SCRIPT="$2"
      shift
      ;;
    --mode)
      require_value "$1" "${2:-}"
      STEP_MODE="$2"
      shift
      ;;
    --timeout-ms)
      require_value "$1" "${2:-}"
      TOTAL_TIMEOUT_MS="$2"
      shift
      ;;
    --step-timeout-ms)
      require_value "$1" "${2:-}"
      STEP_TIMEOUT_MS="$2"
      shift
      ;;
    --robot-id)
      require_value "$1" "${2:-}"
      ROBOT_ID="$2"
      shift
      ;;
    --api-url)
      require_value "$1" "${2:-}"
      API_URL="${2%/}"
      shift
      ;;
    --register-profile)
      REGISTER_PROFILE=true
      ;;
    --ssh-root)
      require_value "$1" "${2:-}"
      SSH_ROOT="$2"
      shift
      ;;
    --compose-override)
      require_value "$1" "${2:-}"
      COMPOSE_OVERRIDE="$2"
      shift
      ;;
    --no-compose)
      APPLY_COMPOSE=false
      ;;
    --yes)
      ACCEPT_HOST_KEY=true
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "未知参数：$1；使用 --help 查看帮助"
      ;;
  esac
  shift
done

[[ -n "$HOST" ]] || fail "必须提供 --host"
[[ "$HOST" =~ ^[A-Za-z0-9_.:%-]+$ && "$HOST" != -* ]] ||
  fail "机器人 host 含有不支持的字符：$HOST"
[[ "$SSH_USER" =~ ^[A-Za-z_][A-Za-z0-9_.-]*$ && "$SSH_USER" != root ]] ||
  fail "SSH 用户必须是非 root 的安全用户名"
is_integer "$SSH_PORT" || fail "SSH 端口必须是数字"
((SSH_PORT >= 1 && SSH_PORT <= 65535)) || fail "SSH 端口必须在 1-65535 之间"
[[ "$PROFILE_NAME" =~ ^[A-Za-z0-9_.-]+$ ]] ||
  fail "方案名称只能包含字母、数字、点、下划线和连字符"
[[ "$PROFILE_NAME" != "." && "$PROFILE_NAME" != ".." ]] ||
  fail "方案名称不能是 . 或 .."
[[ "$STEP_MODE" == wait || "$STEP_MODE" == detached ]] ||
  fail "--mode 只能是 wait 或 detached"
for numeric_value in \
  "$TOTAL_TIMEOUT_MS" \
  "$STEP_TIMEOUT_MS" \
  "$READINESS_RETRY_COUNT" \
  "$READINESS_RETRY_INTERVAL_MS"; do
  is_integer "$numeric_value" || fail "超时和重试参数必须是数字"
done
((TOTAL_TIMEOUT_MS >= 1000 && TOTAL_TIMEOUT_MS <= 3600000)) ||
  fail "总超时必须在 1000-3600000 毫秒之间"
((STEP_TIMEOUT_MS >= 100 && STEP_TIMEOUT_MS <= 3600000)) ||
  fail "步骤超时必须在 100-3600000 毫秒之间"
validate_remote_value "$REMOTE_SCRIPT"
validate_remote_value "$WORKING_DIRECTORY"
validate_remote_value "$LOG_PATH"
[[ "$STEP_MODE" != detached || "$LOG_PATH" == /* ]] ||
  fail "detached 模式的 --log-path 必须是绝对路径"
if [[ -n "$READINESS_SCRIPT" ]]; then
  validate_remote_value "$READINESS_SCRIPT"
fi
[[ "$SSH_ROOT" == /* ]] || fail "--ssh-root 必须是绝对路径"
[[ "$SSH_ROOT" != *$'\n'* && "$SSH_ROOT" != *$'\r'* ]] ||
  fail "--ssh-root 不能包含换行"
if [[ "$COMPOSE_OVERRIDE" != /* ]]; then
  COMPOSE_OVERRIDE="${PROJECT_ROOT}/${COMPOSE_OVERRIDE}"
fi
[[ "$COMPOSE_OVERRIDE" != *$'\n'* && "$COMPOSE_OVERRIDE" != *$'\r'* ]] ||
  fail "--compose-override 不能包含换行"
if [[ "$REGISTER_PROFILE" == true ]]; then
  [[ "$ROBOT_ID" =~ ^[0-9a-fA-F-]{36}$ ]] ||
    fail "--register-profile 需要有效的 --robot-id UUID"
fi

require_command ssh
require_command ssh-keygen
require_command ssh-keyscan

umask 077
mkdir -p "$SSH_ROOT"
chmod 700 "$SSH_ROOT"

KEY_FILE="${SSH_ROOT}/robot_ssh_key"
PUBLIC_KEY="${KEY_FILE}.pub"
KNOWN_HOSTS_FILE="${SSH_ROOT}/robot_known_hosts"
PROFILE_JSON="${SSH_ROOT}/${PROFILE_NAME}.startup-profile.json"

if [[ ! -e "$KEY_FILE" ]]; then
  log "生成 Dispatcher Ed25519 密钥"
  ssh-keygen -q -t ed25519 -f "$KEY_FILE" -N ""
elif [[ ! -f "$KEY_FILE" ]]; then
  fail "密钥路径不是普通文件：$KEY_FILE"
else
  log "复用已有 Dispatcher 密钥：$KEY_FILE"
fi

if [[ ! -f "$PUBLIC_KEY" ]]; then
  log "从私钥生成公钥"
  ssh-keygen -y -f "$KEY_FILE" > "${PUBLIC_KEY}.tmp"
  mv -f "${PUBLIC_KEY}.tmp" "$PUBLIC_KEY"
fi
chmod 600 "$KEY_FILE"
chmod 644 "$PUBLIC_KEY"
touch "$KNOWN_HOSTS_FILE"
chmod 600 "$KNOWN_HOSTS_FILE"

HOST_TOKEN="$HOST"
if ((SSH_PORT != 22)); then
  HOST_TOKEN="[${HOST}]:${SSH_PORT}"
fi

if ssh-keygen -F "$HOST_TOKEN" -f "$KNOWN_HOSTS_FILE" >/dev/null 2>&1; then
  log "known_hosts 已存在目标主机记录：$HOST_TOKEN"
else
  scan_file="$(mktemp)"
  scan_error="$(mktemp)"
  trap 'rm -f "${scan_file:-}" "${scan_error:-}"' EXIT
  log "读取机器人 SSH 主机指纹：$HOST_TOKEN"
  if ! ssh-keyscan -T 10 -p "$SSH_PORT" -H "$HOST" >"$scan_file" 2>"$scan_error"; then
    cat "$scan_error" >&2 || true
    fail "无法读取机器人主机指纹，请检查 IP、端口和 SSH 服务"
  fi
  [[ -s "$scan_file" ]] || fail "机器人没有返回 SSH 主机指纹"
  printf '\n即将记录以下 SSH 主机指纹：\n'
  ssh-keygen -lf "$scan_file" || true
  if [[ "$ACCEPT_HOST_KEY" != true ]]; then
    read -r -p "请确认这是目标机器人，继续写入 known_hosts？[y/N] " answer
    [[ "$answer" =~ ^[Yy]$ ]] || fail "已取消写入主机指纹"
  fi
  cat "$scan_file" >> "$KNOWN_HOSTS_FILE"
  rm -f "$scan_file" "$scan_error"
  scan_file=""
  scan_error=""
  trap - EXIT
  log "已写入 known_hosts：$KNOWN_HOSTS_FILE"
fi

ssh_options=(
  -o BatchMode=yes
  -o StrictHostKeyChecking=yes
  -o "UserKnownHostsFile=${KNOWN_HOSTS_FILE}"
  -o ConnectTimeout=10
  -i "$KEY_FILE"
  -p "$SSH_PORT"
)

if ssh "${ssh_options[@]}" "${SSH_USER}@${HOST}" true >/dev/null 2>&1; then
  log "SSH 密钥登录已经可用"
else
  require_command ssh-copy-id
  log "密钥尚未安装；接下来会提示输入一次机器人密码"
  ssh-copy-id \
    -i "$PUBLIC_KEY" \
    -p "$SSH_PORT" \
    -o StrictHostKeyChecking=yes \
    -o "UserKnownHostsFile=${KNOWN_HOSTS_FILE}" \
    -o ConnectTimeout=10 \
    "${SSH_USER}@${HOST}"
  ssh "${ssh_options[@]}" "${SSH_USER}@${HOST}" true >/dev/null ||
    fail "公钥安装后仍无法使用密钥登录"
  log "公钥已安装，后续不再需要机器人密码"
fi

if [[ -e "$COMPOSE_OVERRIDE" ]] &&
  ! grep -q '^# Generated by scripts/provision_robot_ssh.sh$' "$COMPOSE_OVERRIDE"; then
  fail "已存在非脚本生成的 $COMPOSE_OVERRIDE；请改用 --ssh-root 或先人工合并 Docker 配置"
fi

mkdir -p "$(dirname "$COMPOSE_OVERRIDE")"
cat > "$COMPOSE_OVERRIDE" <<'EOF'
# Generated by scripts/provision_robot_ssh.sh
# Do not commit this file.
# Bind-mounting host 600 keys fails: the container runs as user dispatcher,
# while OpenSSH requires the private key to be owned by that user.
services:
  dispatcher:
    volumes:
      - dispatcher-ssh:/etc/dispatcher/ssh
volumes:
  dispatcher-ssh:
EOF
chmod 600 "$COMPOSE_OVERRIDE"
log "已生成 Docker SSH 卷配置：$COMPOSE_OVERRIDE"

profile_name_json="$(json_escape "$PROFILE_NAME")"
container_key_json="$(json_escape "$CONTAINER_KEY")"
container_known_hosts_json="$(json_escape "$CONTAINER_KNOWN_HOSTS")"
ssh_user_json="$(json_escape "$SSH_USER")"
remote_script_json="$(json_escape "$REMOTE_SCRIPT")"
working_directory_json="$(json_escape "$WORKING_DIRECTORY")"
log_path_json="$(json_escape "$LOG_PATH")"

if [[ "$STEP_MODE" == detached ]]; then
  step_json=$(cat <<EOF
[
    {
      "name": "启动机器人",
      "script": "${remote_script_json}",
      "args": [],
      "working_directory": "${working_directory_json}",
      "environment": {},
      "mode": "detached",
      "log_path": "${log_path_json}",
      "timeout_ms": ${STEP_TIMEOUT_MS}
    }
  ]
EOF
)
else
  step_json=$(cat <<EOF
[
    {
      "name": "执行机器人脚本",
      "script": "${remote_script_json}",
      "args": [],
      "working_directory": "${working_directory_json}",
      "environment": {},
      "mode": "wait",
      "timeout_ms": ${STEP_TIMEOUT_MS}
    }
  ]
EOF
)
fi

readiness_json='[]'
if [[ -n "$READINESS_SCRIPT" ]]; then
  readiness_script_json="$(json_escape "$READINESS_SCRIPT")"
  readiness_json=$(cat <<EOF
[
    {
      "name": "检查机器人就绪",
      "script": "${readiness_script_json}",
      "args": [],
      "working_directory": "${working_directory_json}",
      "environment": {},
      "mode": "wait",
      "retry_count": ${READINESS_RETRY_COUNT},
      "retry_interval_ms": ${READINESS_RETRY_INTERVAL_MS},
      "timeout_ms": 5000
    }
  ]
EOF
)
fi

cat > "$PROFILE_JSON" <<EOF
{
  "name": "${profile_name_json}",
  "description": "由 provision_robot_ssh.sh 生成：${HOST}",
  "enabled": true,
  "ssh_port": ${SSH_PORT},
  "ssh_username": "${ssh_user_json}",
  "credential_reference": "${container_key_json}",
  "known_hosts_reference": "${container_known_hosts_json}",
  "timeout_ms": ${TOTAL_TIMEOUT_MS},
  "steps": ${step_json},
  "readiness_checks": ${readiness_json},
  "stop_steps": []
}
EOF
chmod 600 "$PROFILE_JSON"
log "已生成启动方案 JSON：$PROFILE_JSON"

if [[ "$APPLY_COMPOSE" == true ]]; then
  require_command docker
  docker compose version >/dev/null 2>&1 ||
    fail "需要 Docker Compose v2（docker compose）"
  log "启动或重载 dispatcher，使 SSH 卷生效"
  docker compose up -d dispatcher --force-recreate
  running=false
  for _ in {1..30}; do
    if docker compose exec -T -u 0 dispatcher true >/dev/null 2>&1; then
      running=true
      break
    fi
    sleep 2
  done
  [[ "$running" == true ]] || fail "dispatcher 容器未能进入可执行状态"
  log "把密钥拷入 dispatcher 容器卷，并改为 dispatcher 用户所有"
  docker compose exec -T -u 0 dispatcher mkdir -p /etc/dispatcher/ssh
  docker compose cp "$KEY_FILE" "dispatcher:${CONTAINER_KEY}"
  docker compose cp "$KNOWN_HOSTS_FILE" "dispatcher:${CONTAINER_KNOWN_HOSTS}"
  docker compose exec -T -u 0 dispatcher sh -c \
    "chown dispatcher:dispatcher '${CONTAINER_KEY}' '${CONTAINER_KNOWN_HOSTS}' && \
     chmod 600 '${CONTAINER_KEY}' '${CONTAINER_KNOWN_HOSTS}' && \
     chmod 755 /etc/dispatcher/ssh"
  mounted=false
  for _ in {1..30}; do
    if docker compose exec -T dispatcher sh -c \
      "test -r '${CONTAINER_KEY}' && test -r '${CONTAINER_KNOWN_HOSTS}'" \
      >/dev/null 2>&1; then
      mounted=true
      break
    fi
    sleep 2
  done
  if [[ "$mounted" != true ]]; then
    docker compose exec -T -u 0 dispatcher ls -la /etc/dispatcher/ssh >&2 || true
    docker compose exec -T dispatcher id >&2 || true
    fail "dispatcher 启动后无法读取 SSH 密钥，请检查容器用户权限"
  fi
  log "从 dispatcher 容器测试机器人 SSH"
  docker compose exec -T dispatcher ssh \
    -o BatchMode=yes \
    -o StrictHostKeyChecking=yes \
    -o "UserKnownHostsFile=${CONTAINER_KNOWN_HOSTS}" \
    -o ConnectTimeout=10 \
    -i "$CONTAINER_KEY" \
    -p "$SSH_PORT" \
    "${SSH_USER}@${HOST}" true >/dev/null ||
    fail "宿主机密钥可用，但 dispatcher 容器内 SSH 测试失败"
  log "dispatcher 容器内 SSH 测试通过"
fi

if [[ "$REGISTER_PROFILE" == true ]]; then
  require_command curl
  response_file="$(mktemp)"
  trap 'rm -f "${response_file:-}"' EXIT
  log "通过 API 创建启动方案：${PROFILE_NAME}"
  http_code=""
  for _ in {1..30}; do
    if http_code="$(curl -sS -o "$response_file" -w '%{http_code}' \
      -X POST \
      -H 'Accept: application/json' \
      -H 'Content-Type: application/json' \
      --data-binary "@${PROFILE_JSON}" \
      "${API_URL}/api/v1/robots/${ROBOT_ID}/startup-profiles")"; then
      break
    fi
    sleep 2
  done
  [[ -n "$http_code" ]] || fail "无法访问 API：${API_URL}"
  if [[ "$http_code" != 2?? ]]; then
    cat "$response_file" >&2 || true
    fail "API 创建启动方案失败，HTTP ${http_code}"
  fi
  cat "$response_file"
  rm -f "$response_file"
  response_file=""
  trap - EXIT
  log "启动方案已注册；如果同名方案已存在，后端会创建新的版本"
fi

printf '\nSSH 初始化完成。\n'
printf '私钥：%s\n' "$KEY_FILE"
printf 'known_hosts：%s\n' "$KNOWN_HOSTS_FILE"
printf '容器私钥：%s\n' "$CONTAINER_KEY"
printf '容器 known_hosts：%s\n' "$CONTAINER_KNOWN_HOSTS"
printf '启动方案 JSON：%s\n' "$PROFILE_JSON"
if [[ "$REGISTER_PROFILE" == true ]]; then
  printf '启动方案：已通过 API 注册到机器人 %s\n' "$ROBOT_ID"
else
  printf '下一步：在机器人管理页创建方案，或把 JSON 内容提交到启动方案 API。\n'
fi
printf '仍需手工完成：把机器人绑定到场景，然后在流程中添加 ROBOT_SSH 节点并发布。\n'
