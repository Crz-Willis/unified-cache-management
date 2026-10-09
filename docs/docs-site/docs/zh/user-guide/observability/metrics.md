# UCM 观测能力

UCM 通过 vLLM connector 导出指标，并重用 vLLM 的 Prometheus `/metrics` 端点，不需要单独的导出器、导出模式或服务端口。

建议使用 Prometheus 抓取 vLLM 指标，使用 Grafana 可视化收集的数据。

使用**至少 5 秒**的抓取和仪表板刷新间隔来处理 UCM 指标。本指南中的 Prometheus 和 Metrics-view 示例都使用 5 秒。更短的间隔通常不会使 UCM 指标更新更快。

实际刷新频率还取决于 vLLM。UCM 首先在内部累积指标。新数据仅在 vLLM 处理请求并调用 connector 的 `get_kv_connector_stats()` 方法后才同步到 vLLM 暴露的 Prometheus 指标。**当没有推理请求时，vLLM 不会调用此方法，UCM 指标不会更新。**

## 启用或禁用指标

### 使用内置配置

UCM 指标**默认启用**。当省略 `metrics_config_path` 时，UCM 使用完整的内置指标集。也可以显式启用指标：

```yaml
enable_metrics: true
```

要禁用所有 UCM 指标：

```yaml
enable_metrics: false
```

### 使用自定义配置

要限制导出的指标集或自定义 Histogram 桶，设置以下顶层 UCM 选项：

```yaml
enable_metrics: true
metrics_config_path: "/workspace/unified-cache-management/examples/metrics/metrics_configs.yaml"
```

一旦设置了 `metrics_config_path`，该文件就成为了指标启用列表。只有该文件中定义的指标才会被注册。

指标文件必须存在且可被 vLLM 进程读取。否则，UCM 指标将不会暴露。

## 访问指标

使用 UCM connector 启动 vLLM 并发送至少一个推理请求。然后验证 UCM 指标是否可用：

```bash
curl http://<vllm-ip>:<vllm-port>/metrics | grep '^ucm:'
```

大多数 UCM 指标只有在相应代码路径运行后才会出现。当没有外部存储命中时，可能只存在一小部分指标。

### UCM 指标标签

通过 vLLM connector 导出的每个指标都带有这些标签：

| 标签 | 含义 | 示例 |
| --- | --- | --- |
| `model_name` | vLLM 提供的模型名称，取自 vLLM 模型配置 | `Qwen3-32B` |
| `engine` | 产生指标的 vLLM 引擎；区分同一服务中的 DP 实例 | `engine-0` |
| `worker_rank` | 产生指标的 UCM 进程，对应 TP 实例；worker 使用其分布式 rank，scheduler 使用 `scheduler` | `0`, `1`, `scheduler` |

例如：

```text
ucm:cache_load_bytes_total{model_name="Qwen3-32B",engine="engine-0",worker_rank="0"} 1.048576e+08
```

## Prometheus 和 Grafana 集成

Prometheus 和 Grafana 是观察 UCM 的推荐组合。Prometheus 定期抓取 vLLM 指标端点，存储历史时间序列数据，并提供查询接口。Grafana 查询 Prometheus 并将指标显示为仪表板。

### 配置 Prometheus

如果 Prometheus 已经在抓取 vLLM 的 `/metrics` 端点，则不需要为 UCM 添加额外的抓取任务，因为 vLLM 和 UCM 指标都通过同一端点暴露。

创建 `prometheus.yml` 并配置 Prometheus 抓取 vLLM 服务：

```yaml
global:
  scrape_interval: 5s
  evaluation_interval: 30s

scrape_configs:
  - job_name: vllm
    metrics_path: /metrics
    static_configs:
      - targets:
          - "<vllm-ip>:8000"
```

将抓取目标替换为 Prometheus 容器可访问的引擎地址和实际端口，例如运行快速开始服务的宿主机 IP 与 7800 端口；这里不能用 Prometheus 容器自身的 `127.0.0.1`。

在保存了 `prometheus.yml` 的宿主机目录中创建网络和持久卷，再启动 Prometheus：

```bash
docker network create ucm-monitoring
docker volume create prometheus-data

docker run -d \
  --name prometheus \
  --restart unless-stopped \
  --network ucm-monitoring \
  -p 9090:9090 \
  -v "$PWD/prometheus.yml:/etc/prometheus/prometheus.yml:ro" \
  -v prometheus-data:/prometheus \
  prom/prometheus
```

访问 `http://<prometheus-host>:9090/targets`，确认 `vllm` 抓取目标为 **UP**，并查询 `ucm:` 指标。已有 `ucm-monitoring` 网络时复用它，不必重复创建。

### 安装 Grafana

创建持久卷并启动 Grafana：

```bash
docker volume create grafana-data

docker run -d \
  --name grafana \
  --restart unless-stopped \
  --network ucm-monitoring \
  -p 3000:3000 \
  -v grafana-data:/var/lib/grafana \
  grafana/grafana
```

打开 `http://<grafana-ip>:3000`。首次登录时，用户名和密码都使用 `admin`，然后在提示时更改密码。

### 添加 Prometheus 数据源

在 Grafana 中，进入**连接** → **添加新连接**，搜索 **Prometheus**，并配置：

- Prometheus 服务器 URL：`http://prometheus:9090`
- 身份验证：对于未认证的本地部署选择**无身份验证**
- 选择**保存并测试**并验证 Grafana 可以查询 Prometheus

`prometheus` 主机名由同一个 `ucm-monitoring` 网络解析。Grafana 在其他环境部署时，应填写它实际可访问的 Prometheus 地址。

### 导入 UCM 仪表板

进入**仪表板** → **新建** → **导入**，上传所需的仪表板 JSON 文件，选择 Prometheus 数据源，然后点击**导入**。

UCM 提供这些仪表板：

| 文件 | 用途 |
| --- | --- |
| `examples/metrics/grafana_vllm.json` | vLLM 请求延迟、令牌吞吐量、调度器状态和缓存状态 |
| `examples/metrics/grafana_store.json` | Store 查找、加载、写出、带宽和缓存活动 |
| `examples/metrics/grafana_connector.json` | Connector 查找/加载/保存请求计数、块计数、持续时间、吞吐量和错误 |
| `examples/metrics/grafana_ucm_kv_client.json` | UCM KVClient 请求/Key 速率、各阶段耗时和错误，支持整体及 Worker 视图 |

### 通过 UCM 查看 KVClient 性能

看板标题为 `ucm-kv-client Metrics`，沿用独立看板 `kv-client Metrics` 的命名方式，
并添加 UCM 前缀加以区分。模型发现和筛选控件与 Connector 看板保持一致，
均从 `ucm:*` 指标中发现 job、模型、Engine 和 Worker。
模型可能在 KVClient 指标出现前就显示在列表中；面板需要带有非空 `worker_rank` 的
`ucm:kv_*` 指标才能展示数据。

ASU Store 使用 UCM metrics backend 时，导入 `grafana_ucm_kv_client.json`。
保持 `enable_metrics: true`（默认值），启用 `vllm_connector` consumer，并使用内置指标配置，
或在自定义配置中保留需要的 `kv_*` Counter 和 Histogram。发送实际访问 ASU Store 的推理请求后，
检查 vLLM 的 `/metrics` 端点是否出现 `ucm:kv_*` 指标。

导入后，选择 Prometheus 数据源、目标 vLLM 的抓取 `job` 和 `model_name`。
**View** 支持 **Aggregated**（按模型汇总）和 **Per Worker**
（按 `model_name`、`engine`、`worker_rank` 分开展示）；两种视图均受 Engine 和 Worker 筛选条件影响。
比较 Worker 时，应选择目标部署的 job：所选目标中具有相同 Engine/Worker 标签的序列会被合并。
看板使用默认前缀 `vllm_connector_prefix: "ucm:"`；若自定义前缀，需要同时修改面板查询和变量查询。

看板包含请求/Key 速率、Client/Transport 各阶段耗时、端到端耗时和错误统计。
Node-task Send/Completion 面板汇总每个 Worker 内所有节点的样本；
UCM 不导出 `node_id`，这些面板不能定位具体节点。平均值按样本数加权，
分位数在合并直方图桶后计算。耗时单位为秒，Key 速率不是字节带宽，请求提交速率也不是成功完成速率。

原 `grafana_kv_client.json` 继续用于 standalone backend（`kv:` 前缀，
`source`、`worker_id`、`node_id` 标签）。新看板适配 vLLM connector consumer，
不适配 multiproc consumer，并排除缺少 `worker_rank` 的序列。
若面板无数据，检查前缀、所选 job/model、指标启用列表以及相应 ASU 路径是否实际执行。
速率查询至少需要两次 Prometheus 抓取，耗时分位数需要实际观测样本。

## 用终端查看指标

不部署 Prometheus/Grafana 时，也可以直接查看引擎端点。在 UCM 源码根目录、能够访问该端点的终端中执行：

```bash
python3 -m pip install -e toolkit
ucm-toolkit run metrics-view list-configs
ucm-toolkit run metrics-view check \
  --url http://127.0.0.1:7800/metrics \
  --config store
```

以上查看当前快照；后台采集、时间窗口查询以及 MLA 的 TP 参数见 [metrics-view 完整操作](../../toolkit/user/metrics-view.md)。

## 参考

- [指标定义](metrics-reference.md)
- [Store 健康指标](health-metrics.md)
