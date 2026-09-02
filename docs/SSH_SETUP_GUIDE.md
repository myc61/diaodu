# 机器人 SSH 快速配置与启动操作手册

> 更新日期：2026-08-31
>
> 本手册面向现场部署人员。推荐使用 `scripts/provision_robot_ssh.sh`，把密钥生成、一次性密码引导、主机指纹登记、Docker 挂载和连通性测试压缩成一次操作。

## 1. 功能边界

本项目的 SSH 功能用于从 Dispatcher 调度服务器执行机器人上已经存在的脚本或结构化命令，典型用途是：

- 启动 ROS bringup、导航或机器人业务进程；
- 检查远程程序是否已经就绪；
- 在受控流程中执行固定的维护命令。

它不提供：

- 浏览器任意 SSH 终端；
- 文件上传、下载或远程配置编辑；
- ROS 或系统软件自动安装；
- 流程取消后的自动停止补偿；
- SSH 断线后的 `UNCERTAIN/RECOVERING` 对账。

启动方案只引用远程已有文件，不会把本地 `start.sh` 自动上传到机器人。

## 2. 最快使用方式

### 2.1 前置条件

机器人需要满足：

1. SSH 服务已经启动；
2. 存在低权限 SSH 用户，例如 `naviai`；
3. 机器人上已经存在启动脚本，例如 `/home/naviai/robot/start.sh`；
4. 启动脚本有执行权限；
5. 机器人网络可以从 Dispatcher 所在服务器访问。

脚本会在第一次连接时提示输入一次机器人密码，把 Dispatcher 的公钥安装到机器人。后续使用 SSH Key，不再需要密码。

### 2.2 一条命令初始化

在项目根目录执行：

```bash
./scripts/provision_robot_ssh.sh \
  --host 192.168.1.20 \
  --user naviai \
  --port 22 \
  --profile-name robot_bringup \
  --remote-script ./start.sh \
  --working-directory /home/naviai/robot \
  --readiness-script ./check_ready.sh
```

脚本会自动完成：

1. 在 `~/.config/dispatcher/ssh` 生成或复用一对 Ed25519 密钥；
2. 使用 `ssh-keyscan` 读取机器人主机指纹；
3. 显示指纹并等待现场人员确认；
4. 使用一次机器人密码执行 `ssh-copy-id`；
5. 生成本地 `docker-compose.override.yml`，挂载 dispatcher 专用 SSH 卷；
6. 把私钥和 `known_hosts` 拷入容器，并改为容器用户 `dispatcher` 所有；
7. 生成启动方案 JSON；
8. 重启 Dispatcher；
9. 从 Dispatcher 容器内再次测试 SSH。

如果暂时不希望操作 Docker：

```bash
./scripts/provision_robot_ssh.sh \
  --host 192.168.1.20 \
  --no-compose
```

这会准备密钥、指纹、Compose 覆盖文件和启动方案 JSON，但不会重启或测试容器。

### 2.3 自动注册启动方案

如果机器人已经在系统中创建，并且已经拿到机器人 UUID，可以让脚本直接调用 API 创建启动方案：

```bash
./scripts/provision_robot_ssh.sh \
  --host 192.168.1.20 \
  --user naviai \
  --robot-id 00000000-0000-0000-0000-000000000001 \
  --register-profile
```

API 默认使用：

```text
http://127.0.0.1:8080
```

如果 API 地址不同：

```bash
./scripts/provision_robot_ssh.sh \
  --host 192.168.1.20 \
  --robot-id 00000000-0000-0000-0000-000000000001 \
  --api-url http://127.0.0.1:18080 \
  --register-profile
```

当前系统还没有账号和令牌认证。`--register-profile` 只建议在 Dispatcher 本机或受控管理网执行，不要把 API 暴露到不可信网络。

如果同名方案已经存在，后端会按版本化模型创建新版本；重复执行前请确认是否需要产生新版本。

## 3. 文件和密钥归属

脚本使用一个 Dispatcher 级别的 SSH Key 管理多台机器人，默认目录为：

```text
~/.config/dispatcher/ssh/robot_ssh_key
~/.config/dispatcher/ssh/robot_ssh_key.pub
~/.config/dispatcher/ssh/robot_known_hosts
```

各文件用途如下：

| 文件 | 所在位置 | 用途 |
| --- | --- | --- |
| `robot_ssh_key` | Dispatcher 宿主机 | 登录机器人使用的私钥，只能由 Dispatcher 读取 |
| `robot_ssh_key.pub` | Dispatcher 宿主机和机器人 | 安装到机器人 `authorized_keys` 的公钥 |
| `robot_known_hosts` | Dispatcher 宿主机 | 保存机器人 SSH 主机指纹 |
| `authorized_keys` | 机器人 `naviai` 用户目录 | 授权 Dispatcher 公钥登录 |

私钥和 `known_hosts` 不写入 PostgreSQL，也不会发送给前端。

脚本会生成项目根目录下的：

```text
docker-compose.override.yml
```

该文件为 Dispatcher 挂载专用 Docker 卷 `dispatcher-ssh`（容器内路径 `/etc/dispatcher/ssh`），不保存密钥内容。脚本在容器启动后把宿主机密钥拷入该卷，并 `chown` 给容器用户 `dispatcher`。不能把宿主机 `600` 私钥直接 bind-mount 进容器：宿主机属主和容器用户 UID 不同，`dispatcher` 读不到，OpenSSH 也会拒绝属主不是当前用户的私钥。该文件已加入 `.gitignore`，不要提交到 Git。

## 4. 脚本参数

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--host` | 无，必填 | 机器人 IP 或主机名，同时用于 ROS 和 SSH |
| `--user` | `naviai` | 机器人 SSH 用户，禁止 `root` |
| `--port` | `22` | 机器人 SSH 端口 |
| `--profile-name` | `robot_bringup` | 启动方案名称 |
| `--remote-script` | `./start.sh` | 机器人端启动脚本 |
| `--working-directory` | `/home/naviai/robot` | 机器人端工作目录 |
| `--log-path` | `/tmp/dispatcher-robot-start.log` | detached 模式下机器人端日志路径 |
| `--readiness-script` | 空 | 可选的机器人端就绪检查脚本 |
| `--mode` | `detached` | `wait` 或 `detached` |
| `--timeout-ms` | `120000` | 启动方案总超时 |
| `--step-timeout-ms` | `60000` | 启动步骤超时 |
| `--robot-id` | 空 | 自动注册方案时使用 |
| `--register-profile` | 关闭 | 通过 API 创建启动方案 |
| `--api-url` | `http://127.0.0.1:8080` | API 地址 |
| `--ssh-root` | `~/.config/dispatcher/ssh` | 宿主机密钥目录，建议所有机器人复用同一目录 |
| `--compose-override` | `./docker-compose.override.yml` | 生成的 Compose 覆盖文件 |
| `--no-compose` | 关闭 | 不重启、不测试 Docker |
| `--yes` | 关闭 | 跳过主机指纹确认，不推荐 |

同一 Dispatcher 管理多台机器人时，建议继续使用同一个 `--ssh-root`。脚本会复用同一个私钥，并把不同机器人的指纹追加到同一个 `robot_known_hosts` 文件。

## 5. 页面操作方式

如果没有使用 `--register-profile`，脚本会生成：

```text
~/.config/dispatcher/ssh/robot_bringup.startup-profile.json
```

打开该文件，将内容按页面字段填写。

### 5.1 创建机器人

在“机器人管理”页面创建机器人：

```text
名称：zj-01
IP/主机名：192.168.1.20
rosbridge 端口：9090
ROS 版本：ROS1
ROS 发行版：noetic
```

SSH 使用机器人配置中的 `host`，不需要另填 SSH 主机地址。

保存机器人后，在同一台机器人下新建“受控 SSH 执行方案”。

### 5.2 启动方案基本字段

如果使用脚本生成的默认 Compose 挂载，页面填写：

```text
SSH 用户：naviai
SSH 端口：22
SSH Key Secret 路径：/etc/dispatcher/ssh/robot_ssh_key
known_hosts 路径：/etc/dispatcher/ssh/robot_known_hosts
启用方案：打开
```

注意：

- Key 路径是 Dispatcher 容器内路径，不是宿主机路径；
- `working_directory`、`script` 和 `log_path` 是机器人端路径；
- 页面显示“已配置”只代表数据库有路径引用，不代表文件存在；
- 脚本初始化完成后，容器内测试通过才代表 Key 挂载正确。

### 5.3 执行步骤

最小启动步骤：

```json
[
  {
    "name": "启动机器人",
    "script": "./start.sh",
    "args": [],
    "working_directory": "/home/naviai/robot",
    "environment": {},
    "mode": "detached",
    "log_path": "/tmp/dispatcher-robot-start.log",
    "timeout_ms": 60000
  }
]
```

适合长期运行的 ROS 程序使用 `detached`。它会在机器人上执行 `nohup`，把进程放到后台，并将输出写入机器人上的 `log_path`。

短命令使用 `wait`：

```json
[
  {
    "name": "准备运行目录",
    "command": ["/usr/bin/mkdir", "-p", "/tmp/robot_runtime"],
    "working_directory": "/home/naviai",
    "environment": {},
    "mode": "wait",
    "timeout_ms": 10000
  }
]
```

`command` 的第一个元素是可执行文件，后面元素是参数。`command` 不要再填写非空 `args`。

如果 ROS 环境需要执行 `source /opt/ros/noetic/setup.bash`，推荐把环境初始化写进 `start.sh`，不要在页面中配置 `bash -c` 或 `sh -c`。

### 5.4 就绪检查

只启动后台进程并不代表业务已经可用。推荐配置：

```json
[
  {
    "name": "检查机器人就绪",
    "script": "./check_ready.sh",
    "args": [],
    "working_directory": "/home/naviai/robot",
    "environment": {},
    "mode": "wait",
    "retry_count": 30,
    "retry_interval_ms": 2000,
    "timeout_ms": 5000
  }
]
```

检查脚本退出码为 `0` 时认为已就绪，非 `0` 时按配置重试。`readiness_checks` 不允许使用 `detached`。

如果没有检查脚本，可以填写：

```json
[]
```

但这只能确认启动命令成功返回，不能确认 ROS 节点已经正常工作。

### 5.5 停止步骤

暂时没有自动补偿闭环，停止步骤可以先保存为：

```json
[]
```

即使填写了 `stop_steps`，流程取消、失败或 SSH 断线时也不会自动执行。不要把它当成急停功能。

## 6. 流程编排

完成机器人和启动方案后，还要把机器人绑定到场景：

1. 打开“地图场景/场景工作台”；
2. 选择目标场景；
3. 打开“绑定机器人”；
4. 勾选刚才创建的机器人并保存；
5. 回到“流程编排”；
6. 顶部选择同一个场景。

流程编辑器只会加载“当前场景关联机器人”的启用启动方案。如果机器人没有绑定当前场景，左侧“机器人 SSH / 启动”按钮会被禁用。

推荐最小流程：

```text
START --成功边--> ROBOT_SSH --成功边--> END
```

操作步骤：

1. 点击左侧“机器人 SSH / 启动”；
2. 右侧选择机器人；
3. 右侧选择该机器人的启动方案；
4. 连接 `START` 到 `ROBOT_SSH`；
5. 连接 `ROBOT_SSH` 到 `END`；
6. 保存草稿；
7. 发布；
8. 在运行监控中启动。

前端实际添加的是 `ROBOT_SSH` 节点，后端同时兼容旧的 `ROBOT_STARTUP` 类型。

## 7. SSH 能力模板的使用

如果 SSH 只是启动机器人，优先使用 `ROBOT_SSH` 流程节点。

如果 SSH 是业务动作，例如执行一个固定的检测或维护动作，可以在“能力模板”中选择：

```text
调用方式：SSH
绑定机器人：目标机器人
SSH 执行方案：目标方案
```

保存后可以点击“对选定机器人测试”。这个测试会执行受控 SSH 方案，但不会创建流程运行记录。

SSH 能力也可以被：

- `ROBOT_CAPABILITY` 节点引用；
- 点位动作引用；
- 流程运行调用。

不建议把每个临时命令都建成 SSH 能力。应将固定、审计需要的业务命令做成方案或能力模板。

## 8. 手工验证命令

脚本默认会完成以下验证。需要手工排错时，可以执行：

### 8.1 检查宿主机密钥登录

```bash
ssh \
  -o BatchMode=yes \
  -o StrictHostKeyChecking=yes \
  -o UserKnownHostsFile="$HOME/.config/dispatcher/ssh/robot_known_hosts" \
  -i "$HOME/.config/dispatcher/ssh/robot_ssh_key" \
  -p 22 \
  naviai@192.168.1.20 \
  true
```

### 8.2 检查容器内文件

```bash
docker compose exec dispatcher sh -c \
  'test -r /etc/dispatcher/ssh/robot_ssh_key && echo key-ok'
```

```bash
docker compose exec dispatcher sh -c \
  'test -r /etc/dispatcher/ssh/robot_known_hosts && echo known-hosts-ok'
```

### 8.3 检查容器内 SSH

```bash
docker compose exec dispatcher ssh \
  -o BatchMode=yes \
  -o StrictHostKeyChecking=yes \
  -o UserKnownHostsFile=/etc/dispatcher/ssh/robot_known_hosts \
  -i /etc/dispatcher/ssh/robot_ssh_key \
  -p 22 \
  naviai@192.168.1.20 \
  true
```

## 9. 常见问题

### 左侧启动按钮是灰色

依次检查：

1. 启动方案是否已保存；
2. 启动方案是否启用；
3. 机器人是否绑定到当前场景；
4. 流程编辑器选择的场景是否正确；
5. 页面是否刷新了场景资产。

### `Permission denied`

可能是：

- 机器人 `naviai` 用户不存在；
- 公钥没有安装到 `authorized_keys`；
- `authorized_keys` 或 `.ssh` 权限错误；
- 容器内 `dispatcher` 用户读不到私钥；
- 远程脚本没有执行权限。

### `Host key verification failed`

说明 `known_hosts` 中的主机指纹与机器人当前返回的指纹不一致。确认机器人身份后，可以删除旧记录并重新运行脚本：

```bash
ssh-keygen -R 192.168.1.20 \
  -f "$HOME/.config/dispatcher/ssh/robot_known_hosts"
```

非 22 端口使用：

```bash
ssh-keygen -R '[192.168.1.20]:2222' \
  -f "$HOME/.config/dispatcher/ssh/robot_known_hosts"
```

### `No such file or directory`

检查以下路径是否存在于机器人端：

```text
working_directory
script
log_path 的父目录
```

### detached 成功，但机器人业务没有起来

`detached` 成功只代表后台进程被拉起。请登录机器人检查：

```text
/tmp/dispatcher-robot-start.log
```

同时建议补充 `readiness_checks`，让流程只有在真实业务就绪后才继续。

## 10. 安全要求和已知限制

- 不要把私钥、`known_hosts` 或生成的 Compose 覆盖文件提交到 Git；
- 不要把私钥内容写入 `.env` 或数据库；
- 首次主机指纹应由现场人员确认；
- 使用专用低权限账号，不使用 `root`；
- 不要配置 `bash -c`、`sh -c`、管道、重定向、命令替换或 `sudo`；
- 当前 SSH 执行器使用 `BatchMode`，运行时不支持密码交互；
- `stop_steps` 尚未自动补偿；
- `readiness_checks` 当前是远程脚本，不是 ROS/HTTP 专用探针；
- SSH 断线、重复执行和后端重启恢复仍需要人工处理。

脚本和页面实现分别位于：

- `scripts/provision_robot_ssh.sh`
- `frontend/src/views/RobotManageView.vue`
- `frontend/src/views/WorkflowEditorView.vue`
- `backend/src/remote/controlled_ssh_executor.cpp`
