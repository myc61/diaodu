# 多机器人调度系统

面向 ROS 机器人和现场设备的通用调度平台，提供 Web 工程师工作台，用于机器人接入、地图与点位管理、能力模板、流程编排和运行监控。

当前代码基线采用 C++20、Drogon、PostgreSQL 和 Vue 3，通过 Docker Compose 部署。首个真实机器人基线为 ROS 1 Noetic，机器人通过 rosbridge WebSocket 接入；可选受控 SSH/SFTP 用于按项目显式启用的配置执行，不提供浏览器任意终端。

> 当前仍是工程开发版本，不应直接作为无人值守生产调度系统部署。首版没有账号或令牌，只允许部署在受控局域网。紧急停止、人员安全联锁由机器人安全控制器或安全 PLC 承担，本系统不作为安全认证回路。

## 当前已实现能力

### 机器人与接口

- 机器人连接 CRUD，自动连接/重连，指定位姿话题订阅与缓存。
- ROS 1 Topic 发布、Service 调用、Actionlib goal/feedback/result/cancel 协议骨架。
- rosapi Topic/Service 独立扫描，Action 推导，分类缓存和部分失败提示。
- TypeDef 递归生成 JSON Schema 和默认参数，正确处理标量、动态数组、定长数组和空请求。
- 系统只从配置的定位话题获取位姿，不订阅或解析 `/tf`、`/tf_static`。

### 地图与点位

- PGM/YAML 导入、解析、SHA-256、PNG 预览和不可变地图版本。
- 场景、活动地图、机器人归属、历史地图版本选择。
- 世界坐标/像素坐标双向转换，点位越界校验。
- 点位创建、拖拽、yaw、动作绑定和右键“修改位置、配置动作、发布导航”。
- 二维 yaw 转四元数：`z = sin(yaw / 2)`、`w = cos(yaw / 2)`，导航容差可配置。

### 能力与事件

- 能力模板 CRUD、引用保护删除、指定机器人隔离测试。
- `SERVICE`、`ACTION`、`TOPIC`、`SSH` 四种调用方式。
- Service 仅产生 RESULT 事件；Action 支持 FEEDBACK 和 RESULT；Topic 发布结果不是入站订阅事件。
- 一个能力可定义多个具名事件，支持字段比较、上升沿、冷却和最大触发次数。

### 流程与运行

- 图编辑、保存、发布、人工启动。
- `START`、`END`、`STATION_ACTION`、`ROBOT_CAPABILITY`、`NAVIGATION`、`DELAY`、`MANUAL_CONFIRM`、`EVENT_WAIT`、`ROBOT_STARTUP`、`ROBOT_SSH`、`SUBFLOW` 等节点。
- `WorkflowRun`、`NodeRun`、`CommandRun`、`WorkflowEvent`、`CommandOutbox` 持久化。
- 有限失败重试、有限整流程循环、NodeRun attempt 留痕。
- `MANUAL_CONFIRM` 精确批准/拒绝当前 `node_run_id`，防重复决定并记录操作。
- 运行筛选、终态归档和工程师清理历史。
- 运行监控只读流程图，按真实边事件和 attempt 显示执行路径。
- 跨流程实例关系、`business_key`、持久化 `workflow_signals` 和 `SUBFLOW` 子流程等待。
- 每机器人版本化启动方案和 `ROBOT_STARTUP`/`ROBOT_SSH` 执行。

## 技术栈

- 后端：C++20、CMake、Drogon、Boost.Asio/Beast、PostgreSQL/libpqxx、nlohmann/json。
- 前端：Vue 3、TypeScript、Vite、Vue Router、Pinia、Vue Flow、Konva、Naive UI。
- 部署：Docker Compose，包含 `postgres`、`dispatcher`、`web` 三个服务。

## 目录结构

```text
backend/                 C++20 后端
  include/dispatcher/    头文件
  src/api/               REST API 与路由
  src/db/                PostgreSQL 仓储
  src/ros/               rosbridge、位姿、接口发现
  src/workflow/          状态机执行器与事件
  src/remote/            受控 SSH 执行器
  tests/                 核心测试
frontend/                Vue 3 工作台
db/migrations/           0001 到 0017 数据库迁移
config/                  部署 Profile 与机器人模板
deploy/                  前后端 Dockerfile 与 nginx
```

## 快速开始

```bash
cp .env.example .env
# 按现场修改 .env 中的数据库密码和端口
docker compose build
docker compose up -d
docker compose ps
```

- Web 工程师工作台：<http://127.0.0.1:8088>
- API 健康检查：<http://127.0.0.1:8080/api/v1/health>

## 数据库迁移

数据库迁移文件位于 `db/migrations/`，当前为 `0001_initial.sql` 到 `0017_ssh_capability_kind.sql`。

空 PostgreSQL 数据卷首次初始化时，Compose 会把迁移目录挂载到容器的 `/docker-entrypoint-initdb.d` 并自动按文件名顺序执行，因此新环境不需要手工跑迁移。

已有 PostgreSQL 持久卷不会自动重放后来新增的迁移。升级现有环境时，必须在原数据库上按顺序补跑尚未执行的迁移，不要删除数据卷来“解决”迁移问题。示例：

```bash
docker compose exec postgres \
  psql -U "$POSTGRES_USER" -d "$POSTGRES_DB" \
  -f /docker-entrypoint-initdb.d/0016_robot_ssh_profiles.sql

docker compose exec postgres \
  psql -U "$POSTGRES_USER" -d "$POSTGRES_DB" \
  -f /docker-entrypoint-initdb.d/0017_ssh_capability_kind.sql
```

## 基本使用顺序

1. 在“机器人”页添加机器人：名称、IP/主机名、rosbridge 端口和 WebSocket 路径，并配置定位话题。
2. 在“能力模板”页选择扫描来源机器人，扫描接口，生成参数 Schema，保存能力模板。
3. 在“地图场景”页导入 PGM/YAML，创建点位；点位动作可绑定能力模板。
4. 在“流程编排”页创建流程，使用成功边串联节点；建议最小验证流程为：

```text
START --成功边--> ROBOT_CAPABILITY(/robot_task) --成功边--> END
```

5. 在“运行监控”页人工启动，核对 `WorkflowRun -> NodeRun -> CommandRun` 的请求、关联 ID、结果和状态。

## 流程与事件边界

- `START` 只能通过成功边连接首个业务节点。START 不产生能力事件，禁止用业务事件边连接首个动作。
- 与地点无关的机器人内部业务使用 `ROBOT_CAPABILITY`，不要创建虚假点位。
- 点位动作不自动导航；是否导航由显式 `NAVIGATION` 节点表达。机器人内部自行移动的业务能力标记为 `ROBOT_INTERNAL`。
- Service 没有 Feedback，只有 RESULT；Action/Navigation 才有 FEEDBACK 和 RESULT。
- Topic 发布成功只表示本次发送成功，不代表收到外部消息；Topic 入站事件源目前未实现。
- `EVENT_WAIT` 目前只是按名称等待或人工注入的实验骨架，缺来源过滤、payload 条件、超时和失败出口。
- `MANUAL_CONFIRM` 是正式业务节点，只暂停当前分支，必须按 `run_id/node_run_id` 在运行监控批准或拒绝。
- `DELAY`、重试等待和下一轮循环当前是进程内定时任务，后端重启后不能自动恢复。
- 跨实例关系、`SUBFLOW` 和 `workflow_signals` 已落地；`SUBFLOW` 会启动已发布子流程并等待终态回传。

## 受控 SSH

受控 SSH 按机器人维护版本化启动方案，`robot_startup_profiles` 只保存对密钥和 `known_hosts` 的服务端引用，不保存密钥内容。机器人 SSH 主机直接使用机器人管理中已有的 `host`，默认端口 22，默认用户 `naviai`。

每个方案包含：

- 顺序执行的启动步骤 `steps`；
- 可选 readiness 检查 `readiness_checks`；
- 可选停止步骤 `stop_steps`；
- 总超时 `timeout_ms`。

单个步骤二选一：

- `script`：相对路径如 `./start.sh`，或绝对路径；
- `command`：结构化命令数组，例如 `["roslaunch", "robot", "bringup.launch"]`。

步骤还可以配置 `working_directory`、`environment`、`args`、单步 `timeout_ms` 和 `mode`。`mode=wait` 等待命令结束；`mode=detached` 使用 `nohup` 后台启动，必须填写绝对 `log_path`。

脚本步骤示例：

```json
{
  "name": "启动导航",
  "script": "./start.sh",
  "args": [],
  "working_directory": "/home/naviai/robot",
  "environment": {},
  "mode": "detached",
  "log_path": "/tmp/dispatcher-navigation.log",
  "timeout_ms": 60000
}
```

结构化命令示例：

```json
{
  "name": "启动 bringup",
  "command": ["roslaunch", "robot", "bringup.launch"],
  "working_directory": "/home/naviai",
  "environment": {},
  "mode": "detached",
  "log_path": "/tmp/dispatcher-bringup.log",
  "timeout_ms": 60000
}
```

安全边界：

- 不提供浏览器任意 SSH 终端。
- 禁止 root 账号。
- 禁止 `sh`、`bash`、`sudo`、`su` 等解释器或提权程序。
- 禁止管道、重定向、命令替换、换行和路径穿越。
- 使用 `BatchMode`，严格校验 `known_hosts`。
- 前端不会收到私钥或 `known_hosts` 服务端路径，只返回是否已配置。

`stop_steps` 已保存，但尚未形成流程取消或失败时的自动停止补偿闭环。

## SSH Key 与 known_hosts 部署

SSH 私钥和 `known_hosts` 必须由管理员在宿主机放置，并通过 Docker Secret 或只读 bind mount 注入 dispatcher 容器。默认允许的服务端引用根目录为 `/run/secrets:/etc/dispatcher/ssh`，可用 `DISPATCHER_SSH_SECRET_ROOTS` 扩展。

启用真实 SSH 前：

1. 在宿主机安全目录准备低权限账号的私钥。
2. 生成目标机器人主机密钥记录，例如：

```bash
ssh-keyscan -p 22 <机器人IP> > /secure/path/dispatcher-known_hosts
```

3. 将私钥和 `known_hosts` 以只读方式挂载到容器的 `/etc/dispatcher/ssh` 或 `/run/secrets`，并确保容器内运行用户 `dispatcher` 可以读取。
4. 不要把真实私钥或 `known_hosts` 提交到仓库，也不要写入 `.env`。

当前 `docker-compose.yml` 只配置了 `DISPATCHER_SSH_SECRET_ROOTS`，尚未包含 `/etc/dispatcher/ssh` 的持久挂载；启用真实 SSH 前需要按现场部署方式补充 bind mount 或 Compose secrets。

## 构建与测试

核心后端本地验证：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/backend/dispatcher_core_cli --self-check
```

完整服务镜像构建：

```bash
docker compose build dispatcher web
```

如果宿主机安装了 npm，也可以单独验证前端：

```bash
npm --prefix frontend run build
```

## 当前限制与待办

尚未形成可靠生产闭环：

- 业务成功/失败条件求值：当前主要依赖 rosbridge `success`，未按响应字段执行 `success_condition`/`failure_condition`。
- 失败、超时、取消边和节点级超时/取消确认不完整。
- DELAY、重试等待和下一轮循环是进程内定时任务，后端重启后不能恢复。
- 资源锁尚未覆盖命令下发到 result/取消/人工处置的完整生命周期。
- 幂等、去重、断线对账和 `UNCERTAIN/RECOVERING` 未完成。
- 任意 ROS Topic 入站消息触发流程未实现。
- 能力不可变版本和已发布流程能力快照未实现。
- 运行态 WebSocket 推送尚未实现，前端主要轮询。
- 设备 HTTP/WebSocket/MQTT 适配器、场景切换 HARD Saga、项目包和插件未实现。
- SSH 断线对账、幂等、自动停止补偿和 ROS/HTTP 专用 readiness 检查未完成。

最近的优先工作是完成真实 ROS 1 `/robot_task` 人工验收，并补齐业务结果判定、失败出口和可重启恢复的定时任务。
