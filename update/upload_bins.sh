#!/data/data/com.termux/files/usr/bin/bash
# 用法：bash upload_bins.sh

TOKEN="${GH_TOKEN}"                 # 从环境变量读，别写死
OWNER="beier13140317-jpg"
REPO="suliu-YGX"
BRANCH="main"
LOCAL_DIR="/data/data/com.termux/files/home/云更"
  # 你放二进制的地方
REMOTE_DIR="update"                 # 仓库里放二进制的目录

if [ -z "$TOKEN" ]; then
  echo "❌ 先 export GH_TOKEN=ghp_新token"
  exit 1
fi

python3 - "$TOKEN" "$OWNER" "$REPO" "$BRANCH" "$LOCAL_DIR" "$REMOTE_DIR" <<'PY'
import sys, os, base64, json, urllib.request

token, owner, repo, branch, local_dir, remote_dir = sys.argv[1:7]
api = "https://api.github.com"
hdr = {
    "Authorization": f"token {token}",
    "Content-Type": "application/json",
    "Accept": "application/vnd.github.v3+json",
}

def put_file(local_path, remote_path):
    name = os.path.basename(local_path)
    with open(local_path, "rb") as f:
        b64 = base64.b64encode(f.read()).decode()

    # 先查远端 sha（存在则覆盖，不存在则新建）
    url = f"{api}/repos/{owner}/{repo}/contents/{remote_path}?ref={branch}"
    req = urllib.request.Request(url, headers=hdr)
    try:
        with urllib.request.urlopen(req) as r:
            sha = json.load(r).get("sha")
    except urllib.error.HTTPError as e:
        if e.code == 404:
            sha = None
        else:
            print(f"⚠️ {name} 查 sha 失败: {e.code}")
            return

    body = {
        "message": f"upload {name}",
        "content": b64,
        "branch": branch,
    }
    if sha:
        body["sha"] = sha

    req = urllib.request.Request(
        f"{api}/repos/{owner}/{repo}/contents/{remote_path}",
        data=json.dumps(body).encode(),
        headers=hdr,
        method="PUT",
    )
    try:
        with urllib.request.urlopen(req) as r:
            print(f"✅ {name} -> {remote_path} ({r.status})")
    except urllib.error.HTTPError as e:
        print(f"❌ {name} 上传失败: {e.code} {e.read().decode()[:200]}")

# 遍历本地目录里的文件（不递归子目录，要递归改 os.walk）
for fn in sorted(os.listdir(local_dir)):
    lp = os.path.join(local_dir, fn)
    if os.path.isfile(lp):
        put_file(lp, f"{remote_dir}/{fn}")

PY