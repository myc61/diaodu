# 多机器人调度系统

面向 ROS 机器人和现场设备的通用调度平台。当前代码基线采用 C++20、Drogon、PostgreSQL、Vue 3、Vue Flow、Konva 和 Docker Compose，已完成机器人/地图/能力/流程的基础闭环，正在进行真实 ROS1 业务链路与可靠执行器验收。

> 当前仍是工程开发版本，不应直接作为无人值守生产调度系统部署。首版免登录，只允许运行在受控局域网。

## 文档入口

| 文档 | 用途 |
| --- | --- |
| [AI 交接](./docs/AI_WORK_LOG.md) | 新窗口或其他 Agent 的第一阅读入口，记录代码现状、关键约束、验证证据和续作规则 |
| [需求规格](./docs/REQUIREMENTS.md) | 产品范围、功能需求、非功能需求和验收场景 |
| [技术方案](./docs/TECHNICAL_DESIGN.md) | 架构、数据、ROS 契约、执行语义、并发、部署和测试设计 |
| [任务进度](./docs/TASK_PROGRESS.md) | 截至当前代码基线已经实现、验证和仍受限的能力 |
| [任务待办](./docs/TASK_BACKLOG.md) | 按 P0/P1/P2 排序的可执行任务与验收口径 |
| [优化方向](./docs/OPTIMIZATION_ROADMAP.md) | 不阻塞当前验收的中长期架构、体验和产品化优化 |

文档口径发生冲突时，按以下顺序处理：代码和数据库迁移是实现事实；需求规格决定目标；技术方案决定设计；任务进度描述当前能力；任务待办决定近期执行顺序；AI 交接补充现场上下文和安全约束。

## 容器启动

```bash
cp .env.example .env
docker compose build
docker compose up -d
docker compose ps
```

- Web 工程师工作台：<http://127.0.0.1:8088>
- API 健康检查：<http://127.0.0.1:8080/api/v1/health>

已有 PostgreSQL 持久卷不会自动重放后来新增的迁移（当前新增 `0015`）。升级现有环境前，先阅读 [AI 交接](./docs/AI_WORK_LOG.md) 中的迁移说明。

受控 SSH 启动只接受后端保存的启动方案：脚本默认必须位于 `/opt/robot` 或
`/usr/local/lib/dispatcher/scripts`，凭据只允许来自 `/run/secrets` 或
`/etc/dispatcher/ssh`。可在 `.env` 中通过 `DISPATCHER_SSH_ALLOWED_SCRIPT_ROOTS`
和 `DISPATCHER_SSH_SECRET_ROOTS` 显式扩展白名单；浏览器不能提交任意 SSH/Shell 命令，
也不会收到私钥或 `known_hosts` 路径。

## 本地核心验证

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/backend/dispatcher_core_cli --self-check
```

前端验证：

```bash
npm --prefix frontend run build
```

完整服务镜像验证（只构建，不启动机器人任务）：

```bash
docker compose build dispatcher web
```

## 当前首要验收

在工程师确认真实机器人请求参数后，人工运行以下流程并从运行监控核对 `WorkflowRun → NodeRun → CommandRun`：

```text
START --成功边--> ROBOT_CAPABILITY(/robot_task) --成功边--> END
```

禁止从 START 使用业务事件边；START 本身不会产生能力事件。未经工程师明确确认，不得自动调用真实机器人的 `/robot_task` 或其他有业务副作用的接口。
