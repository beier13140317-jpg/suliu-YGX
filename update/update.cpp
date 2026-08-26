#include <iostream>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <openssl/sha.h>

using namespace std;

// ==================== 配置区 ====================
static const char* UPDATE_BASE = "https://gh-proxy.com/https://raw.githubusercontent.com/beier13140317-jpg/suliu-YGX/main/";
static const string APP_VER = "1.0.4";
static const string APP_NAME = "苏六";
// ================================================

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    string* str = static_cast<string*>(userp);
    str->append(static_cast<char*>(contents), realsize);
    return realsize;
}

// 下载文件，返回 true 成功
static bool DownloadFile(const string& url, const string& out_path) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    FILE* fp = fopen(out_path.c_str(), "wb");
    if (!fp) { curl_easy_cleanup(curl); return false; }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Su6-Updater/1.0");

    CURLcode res = curl_easy_perform(curl);
    fclose(fp);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK);
}

// 计算文件 SHA256，返回十六进制字符串
static string CalcSHA256(const string& filepath) {
    FILE* fp = fopen(filepath.c_str(), "rb");
    if (!fp) return "";

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        SHA256_Update(&ctx, buf, n);
    }
    fclose(fp);

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &ctx);

    char hex[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hex + i * 2, "%02x", hash[i]);
    }
    return string(hex);
}

// 解析 version.txt：版本|文件名|SHA256
static bool ParseVersionTxt(const string& content, string& ver, string& name, string& sha) {
    size_t p1 = content.find('|');
    if (p1 == string::npos) return false;
    size_t p2 = content.find('|', p1 + 1);
    if (p2 == string::npos) return false;

    ver = content.substr(0, p1);
    name = content.substr(p1 + 1, p2 - p1 - 1);
    sha = content.substr(p2 + 1);

    // 去换行
    if (!sha.empty() && sha[sha.size()-1] == '\n') sha.pop_back();
    if (!sha.empty() && sha[sha.size()-1] == '\r') sha.pop_back();

    return true;
}

// 比较版本号：返回 true 如果 remote > local
static bool IsNewer(const string& remote, const string& local) {
    // 简单按点分割比较
    auto split = [](const string& s) {
        vector<int> parts;
        size_t start = 0, end = 0;
        while ((end = s.find('.', start)) != string::npos) {
            parts.push_back(atoi(s.substr(start, end - start).c_str()));
            start = end + 1;
        }
        parts.push_back(atoi(s.substr(start).c_str()));
        return parts;
    };

    vector<int> r = split(remote), l = split(local);
    int max_len = max(r.size(), l.size());
    r.resize(max_len, 0);
    l.resize(max_len, 0);

    for (int i = 0; i < max_len; i++) {
        if (r[i] > l[i]) return true;
        if (r[i] < l[i]) return false;
    }
    return false; // 相等
}

// 主更新函数，在菜单里调用
void CheckUpdate() {
    cout << "\n🔍 正在检查更新...\n" << endl;

    // 1. 下载 version.txt
    string ver_url = string(UPDATE_BASE) + "version.txt";
    string ver_content;

    CURL* curl = curl_easy_init();
    if (!curl) {
        cout << "❌ CURL 初始化失败" << endl;
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, ver_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ver_content);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Su6-Updater/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || ver_content.empty()) {
        cout << "❌ 无法获取版本信息，请检查网络" << endl;
        return;
    }

    // 2. 解析
    string remote_ver, remote_name, remote_sha;
    if (!ParseVersionTxt(ver_content, remote_ver, remote_name, remote_sha)) {
        cout << "❌ version.txt 格式错误" << endl;
        return;
    }

    cout << "📌 当前版本: " << APP_VER << endl;
    cout << "📌 最新版本: " << remote_ver << endl;

    // 3. 比较
    if (!IsNewer(remote_ver, APP_VER)) {
        cout << "✅ 已是最新版本！" << endl;
        return;
    }

    cout << "🚀 发现新版本 " << remote_ver << "，开始更新...\n" << endl;

    // 4. 下载新文件
    string dl_url = string(UPDATE_BASE) + "update/" + APP_NAME;
    string tmp_path = "/data/data/com.termux/files/home/验证/update_tmp_苏六";

    cout << "⬇️ 正在下载新版本..." << endl;
    if (!DownloadFile(dl_url, tmp_path)) {
        cout << "❌ 下载失败" << endl;
        return;
    }

    // 5. 校验 SHA256
    cout << "🔐 正在校验文件完整性..." << endl;
    string file_sha = CalcSHA256(tmp_path);
    if (file_sha != remote_sha) {
        cout << "❌ 文件校验失败！(SHA256 不匹配)" << endl;
        cout << "   期望: " << remote_sha << endl;
        cout << "   实际: " << file_sha << endl;
        remove(tmp_path.c_str());
        return;
    }

    // 6. 替换
    string current_path = "/data/data/com.termux/files/home/验证/苏六";
    chmod(tmp_path.c_str(), 0755);

    // 备份旧版
    string backup_path = current_path + ".bak";
    remove(backup_path.c_str());
    rename(current_path.c_str(), backup_path.c_str());

    // 覆盖新版本
    if (rename(tmp_path.c_str(), current_path.c_str()) != 0) {
        cout << "❌ 替换文件失败" << endl;
        // 恢复备份
        rename(backup_path.c_str(), current_path.c_str());
        return;
    }

    cout << "✅ 更新成功！新版本: " << remote_ver << endl;
    cout << "🔄 请重新启动工具" << endl;
}
