# Git 推送配置说明

本仓库推送用 **SSH**，`~/.git-credentials` 里另存一份 HTTPS token 作为备用。

## 一、SSH（推荐）

### 1. 检查有没有现成的 key

```bash
ls -la ~/.ssh/id_ed25519*
```

有就跳过下一步。

### 2. 生成（如果没有）

```bash
ssh-keygen -t ed25519 -C "your_email@example.com"
# 一路回车，密码可留空
```

### 3. 把公钥加到 GitHub

```bash
cat ~/.ssh/id_ed25519.pub
```

复制输出，粘贴到 GitHub → Settings → SSH and GPG keys → New SSH key。

或者用 API 添加（把 TOKEN 换成你的 PAT）：

```bash
curl -X POST \
  -H "Authorization: token $TOKEN" \
  -H "Accept: application/vnd.github+json" \
  https://api.github.com/user/keys \
  -d "{\"title\":\"$(hostname)\",\"key\":\"$(cat ~/.ssh/id_ed25519.pub)\"}"
```

返回 `HTTP 201` 且 `"verified": true` 即为成功。

### 4. 验证

```bash
ssh -T git@github.com
# 期望：Hi <用户名>! You've successfully authenticated...
```

### 5. 仓库 remote 用 SSH 地址

```bash
git remote set-url origin git@github.com:<用户名>/<仓库>.git
```

## 二、HTTPS token 备用方案

### 1. 写凭据文件

```bash
printf 'https://<用户名>:<TOKEN>@github.com\n' > ~/.git-credentials
```

**注意**：一个账号一行，格式是 `协议://用户名:token@主机`。
如果文件里已有其他站点的凭据，用 `>>` 追加而不是 `>` 覆盖。

### 2. 启用 store helper

```bash
git config --global credential.helper store
```

之后 HTTPS 操作会自动读这个文件，不用再输 token。

### 3. 收紧权限

Linux / macOS：
```bash
chmod 600 ~/.git-credentials
```

Windows（Git Bash 下 `chmod` 对 NTFS 无效，要用 icacls）：
```bash
F=~/.git-credentials
icacls "$F" /inheritance:r /grant:r "$(whoami):F"
```

## 三、本机环境的两个坑

### 代理端口是动态的

这台机器访问 GitHub 要走本地代理，但**端口每次会话都可能变**，不能写死。

```bash
# 动态读取当前端口
P=$(env | grep -m1 -oP '(?<=https_proxy=http://)[0-9.:]+' | tr -d '\r')
```

HTTPS 走代理时这样用：
```bash
git -c http.proxy="http://$P" -c http.sslVerify=false push origin main
```

`http.sslVerify=false` 是因为代理做了 TLS 中间人，本地 CA 验不过。
**SSH 不需要代理**（直连 22 端口可用），所以 SSH 方式更省事：

```bash
# SSH 直连即可，无需任何代理参数
git push origin main
```

### 代理环境变量会干扰 SSH

如果 shell 里设了 `NO_PROXY` / `no_proxy` 且不含 `github.com`，
某些工具会尝试走代理。推之前可以先清掉：

```bash
unset NO_PROXY no_proxy
```

## 四、当前状态

| 项目 | 配置 |
|---|---|
| remote | `git@github.com:aalexantt/t113-h264-player.git`（SSH）|
| SSH key | `~/.ssh/id_ed25519`（ed25519，已加到 GitHub，`verified: true`）|
| 备用凭据 | `~/.git-credentials`，含 `https://aalexantt:<token>@github.com` |
| credential.helper | `store` |
| 代理 | 仅 HTTPS 需要，端口从 `$https_proxy` 动态取；SSH 直连不需要 |

## 五、安全提醒

- `~/.git-credentials` 是**明文**存储 token。确保权限只有自己可读。
- token 一旦出现在聊天记录、日志、截图里，就当它已泄露，**立刻去 GitHub 撤销**：
  Settings → Developer settings → Personal access tokens → 删除。
- token 不要写进仓库文件、`.git/config`、或任何会提交的地方。
  检查：`grep -r "ghp_" .git/config` 应该没有结果。
- 能用 SSH 就用 SSH，密钥比 token 更难泄露，且可以单独撤销。