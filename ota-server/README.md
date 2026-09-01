# TC1 OTA Server

自动更新固件的HTTP服务器，配合GitHub Webhook实现自动发布。

## 部署

```bash
# 启动服务
docker-compose up -d

# 查看日志
docker-compose logs -f
```

## 配置GitHub Webhook

1. 进入GitHub仓库 → Settings → Webhooks → Add webhook
2. Payload URL: `http://你的服务器IP:8081/webhook`
3. Content type: `application/json`
4. Secret: 设置一个密钥（可选）
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
