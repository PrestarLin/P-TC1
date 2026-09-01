# TC1 OTA Server

自动更新固件的HTTP服务器，配合GitHub Webhook实现自动发布。

## 部署

### 方式1：使用预构建镜像（推荐）

```bash
docker-compose up -d
```

### 方式2：本地构建

```bash
docker build -t tc1-ota-server .
docker run -d -p 8081:8081 -v ./data:/data tc1-ota-server
```

## 配置GitHub Webhook

1. 进入GitHub仓库 → Settings → Webhooks → Add webhook
2. Payload URL: `http://你的服务器IP:8081/webhook`
3. Content type: `application/json`
4. Secret: 设置一个密钥（可选，需设置环境变量 `WEBHOOK_SECRET`）
5. Events: 选择 "Releases"

## 手动上传固件

```bash
# 复制固件到data目录
cp firmware.bin ./data/firmware.bin
echo "v4.1.25" > ./data/version.txt
```

## API

| 端点 | 方法 | 说明 |
|------|------|------|
| `/version` | GET | 返回当前版本号 |
| `/firmware` | GET | 下载固件文件 |
| `/webhook` | POST | GitHub webhook接收 |

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `OTA_PORT` | 8081 | 服务端口 |
| `WEBHOOK_SECRET` | 空 | Webhook密钥 |
