// auth.cpp —— 编译后删源码，不公开
#include "Auth.h"
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <ctime>
#include <curl/curl.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <openssl/sha.h>

using namespace std;

// ==================== 颜色/样式 ====================
#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define DIM     "\033[2m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define CYAN    "\033[36m"
#define BG_DARK "\033[48;5;235m"

// ==================== 配置 ====================
static const unsigned char encrypted_url[] = {
    0x32,0x2e,0x2e,0x2a,0x29,0x60,0x75,0x75,0x2d,0x2d,0x2d,0x74,
    0x31,0x3f,0x23,0x2e,0x74,0x39,0x34,0x75,0x31,0x3b,0x37,0x33,
    0x75,0x29,0x2f,0x29,0x33,0x22,0x6d,0x62,0x63,0x6b,0x75,0x39,
    0x32,0x3f,0x39,0x31,0x74,0x2a,0x32,0x2a,
};
static const size_t encrypted_len = 44;
static const unsigned char XOR_KEY = 0x5A;

static const string APP = "a";

#ifdef _WIN32
static const string CACHE_DIR  = "C:\\Users\\Public\\苏六";
static const string CACHE_FILE = CACHE_DIR + "\\.auth_cache.dat";
static const string HWID_FILE  = CACHE_DIR + "\\.su6_hwid";
#else
static const string CACHE_DIR  = "/storage/emulated/0/苏六";
static const string CACHE_FILE = CACHE_DIR + "/.auth_cache.dat";
static const string HWID_FILE  = CACHE_DIR + "/.su6_hwid";
#endif

// ==============================================

static atomic<bool> g_hb_running{false};
static string g_hb_card, g_hb_mac;
static atomic<bool> g_hb_failed{false};

// ==================== 内部工具 ====================
static size_t WriteCb(void* p, size_t sz, size_t nm, string* o) {
    o->append((char*)p, sz * nm); return sz * nm;
}

static string DecryptUrl() {
    string s((const char*)encrypted_url, encrypted_len);
    for (char& c : s) c ^= XOR_KEY;
    return s;
}

// ---------- HWID：SHA256 指纹 ----------
static string sha256_hex(const string& s) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)s.c_str(), s.size(), hash);
    char buf[3];
    string out;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        snprintf(buf, sizeof(buf), "%02x", hash[i]);
        out += buf;
    }
    return out;
}
static string Trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == string::npos) return "";
    return s.substr(a, b - a + 1);
}
static string RunCmd(const string& cmd) {
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return "";
    char buf[512] = {0};
    string out;
    while (fgets(buf, sizeof(buf), fp)) out += buf;
    pclose(fp);
    return Trim(out);
}

static string GetHWID() {
    // 已有指纹文件 → 直接读（稳定，换机器大概率不同）
    ifstream f(HWID_FILE);
    if (f) {
        string saved; getline(f, saved);
        if (saved.size() >= 32) return saved;
    }

    string model  = RunCmd("getprop ro.product.model");
    string brand  = RunCmd("getprop ro.product.brand");
    string serial = RunCmd("getprop ro.serialno");

    string seed = model + "|" + brand + "|" + serial + "|";
    if (seed.size() < 12) seed = "termux|android|";  // 全空时兜底

    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    seed += to_string(ts.tv_sec) + to_string(ts.tv_nsec);

    string hwid = sha256_hex(seed).substr(0, 32);

    ofstream o(HWID_FILE);
    if (o) o << hwid;
#ifdef _WIN32
    SetFileAttributesA(HWID_FILE.c_str(), FILE_ATTRIBUTE_HIDDEN);
#endif
    return hwid;
}
// ----------------------------------------

static bool HttpGet(const string& url, string& out) {
    CURL* c = curl_easy_init();
    if (!c) return false;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, WriteCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode r = curl_easy_perform(c);
    curl_easy_cleanup(c);
    return r == CURLE_OK;
}

// ==================== 缓存 ====================
static void SaveCache(const string& sign, long long expire_ts) {
    string d = sign + "|" + to_string(expire_ts);
    for (char& c : d) c ^= 0x3C;
    ofstream f(CACHE_FILE, ios::binary);
    if (f) f.write(d.c_str(), d.size());
#ifdef _WIN32
    SetFileAttributesA(CACHE_FILE.c_str(), FILE_ATTRIBUTE_HIDDEN);
#endif
}
static bool LoadCache(string& sign, int& remain_sec) {
    ifstream f(CACHE_FILE, ios::binary);
    if (!f) return false;
    string d((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
    for (char& c : d) c ^= 0x3C;
    size_t sep = d.find('|');
    if (sep == string::npos) return false;
    sign = d.substr(0, sep);
    long long exp = 0;
    try { exp = stoll(d.substr(sep + 1)); } catch(...) { return false; }
    remain_sec = (int)(exp - (long long)time(nullptr));
    return remain_sec > 0 && !sign.empty();
}

// ==================== 心跳 ====================
static void HeartbeatLoop() {
    const int HB_SEC = 52, MAX_FAIL = 3;
    int fail_cnt = 0;
    while (g_hb_running) {
        this_thread::sleep_for(chrono::seconds(HB_SEC));
        if (!g_hb_running) break;

        string base = DecryptUrl();
        string hburl = base + "?card=" + g_hb_card
                            + "&mac=" + g_hb_mac
                            + "&app=" + APP
                            + "&api=heartbeat";

        string reply; bool net_ok = HttpGet(hburl, reply);
        if (!net_ok) fail_cnt++;
        else if (reply.find("too_frequent") != string::npos) fail_cnt = 0;
        else if (reply.rfind("ok|", 0) == 0 || (reply.size() > 0 && reply[0] == '0')) fail_cnt = 0;
        else fail_cnt++;

        if (fail_cnt >= MAX_FAIL) { g_hb_running = false; g_hb_failed = true; return; }
    }
}
bool Auth_IsAlive() { return g_hb_running && !g_hb_failed; }

// ==================== 对外 API ====================
bool Auth_Check(string& msg) {

#ifdef _WIN32
    system("if not exist \"C:\\Users\\Public\\苏六\" mkdir \"C:\\Users\\Public\\苏六\"");
#else
    system("mkdir -p /storage/emulated/0/苏六");
#endif

    curl_global_init(CURL_GLOBAL_ALL);

    // 1. 缓存优先
        string sign; int remain;
    if (LoadCache(sign, remain)) {
        cout << "\n" << BG_DARK;
        cout << CYAN "╔══════════════════════════════════════╗" RESET "\n";
        cout << CYAN "║" RESET BOLD "           苏六工具 - 网络验证        " CYAN "║" RESET "\n";
        cout << CYAN "╠══════════════════════════════════════╣" RESET "\n";
        cout << CYAN "║" RESET GREEN "  ✅ 缓存验证通过！                   " CYAN "║" RESET "\n";

        if (remain > 500000) {
            // 永久卡（expire_ts 非常大，剩余秒数远超普通卡）
            cout << CYAN "║" RESET "  ⏱  剩余: " YELLOW "永久" RESET "       " CYAN "║" RESET "\n";
        } else {
            int hours = remain / 3600, mins = (remain % 3600) / 60;
            cout << CYAN "║" RESET "  ⏱  剩余: " YELLOW << hours << "小时" << mins << "分钟" <<RESET            << string(22 - to_string(hours).length() - to_string(mins).length(), ' ') << CYAN "║" RESET "\n";
        }

        cout << CYAN "║" RESET "  📴 离线模式                         " CYAN "║" RESET "\n";
        cout << CYAN "║" RESET DIM "  正在进入工具...                     " CYAN "║" RESET "\n";
        cout << CYAN "╚══════════════════════════════════════╝" RESET "\n";
        cout.flush();
        this_thread::sleep_for(chrono::milliseconds(1200));

        if (remain > 500000) {
            msg = "✅ 缓存验证通过（永久授权，离线模式）";
        } else {
            int hours = remain / 3600, mins = (remain % 3600) / 60;
            msg = "✅ 缓存验证通过（剩余 " + to_string(hours) + "小时" + to_string(mins) + "分钟，离线模式）";
        }

        g_hb_mac = GetHWID();
        g_hb_running = true;
        thread(HeartbeatLoop).detach();
        return true;
    }


    // 2. 在线验证界面
    cout << "\n" << BG_DARK;
    cout << CYAN "╔══════════════════════════════════════╗" RESET "\n";
    cout << CYAN "║" RESET BOLD "           苏六工具 - 网络验证        " CYAN "║" RESET "\n";
    cout << CYAN "╠══════════════════════════════════════╣" RESET "\n";
    cout << CYAN "║" RESET "                                      " CYAN "║" RESET "\n";
    cout << CYAN "║" RESET "   " BOLD "请输入卡密:" RESET "                        " CYAN "║" RESET "\n";
    cout << CYAN "║" RESET "   " CYAN "█" RESET "                                  " CYAN "║" RESET "\n";

    cout << CYAN "║" RESET "                                      " CYAN "║" RESET "\n";
    cout << CYAN "║" RESET DIM "  [提示] 卡密区分大小写               " CYAN "║" RESET "\n";
    cout << CYAN "╚══════════════════════════════════════╝" RESET "\n";

    cout << "\n  " YELLOW "▶ 请输入卡密: " RESET;
    cout.flush();
    string card;
    getline(cin, card);
    if (card.empty()) { curl_global_cleanup(); return false; }

    cout << CYAN "  ⏳ 正在验证";
    cout.flush();
    for (int i = 0; i < 3; i++) {
        this_thread::sleep_for(chrono::milliseconds(400));
        cout << "."; cout.flush();
    }
    cout << "\n\n";

    string mac = GetHWID();
    string base = DecryptUrl();
    string url = base + "?card=" + card + "&mac=" + mac + "&app=" + APP;

    string reply;
    if (!HttpGet(url, reply)) {
        cout << CYAN "╔══════════════════════════════════════╗" RESET "\n";
        cout << CYAN "║" RESET RED "  ❌ 网络错误，请检查网络连接       " CYAN "║" RESET "\n";
        cout << CYAN "╚══════════════════════════════════════╝" RESET "\n";
        cout << "\n  "; { int _ = getchar(); (void)_; }
        curl_global_cleanup();
        return false;
    }
   


    bool pass = (reply.rfind("ok|", 0) == 0) || (reply.size() > 0 && reply[0] == '0');
    if (!pass) {
        cout << CYAN "╔══════════════════════════════════════╗" RESET "\n";
        cout << CYAN "║" RESET RED "  ❌ 验证失败                       " CYAN "║" RESET "\n";
        cout << CYAN "║" RESET "  卡密无效或已过期                   " CYAN "║" RESET "\n";
        cout << CYAN "║" RESET DIM "  按回车退出...                  " CYAN "║" RESET "\n";
        cout << CYAN "╚══════════════════════════════════════╝" RESET "\n";
        cout << "\n  "; { int _ = getchar(); (void)_; }
        curl_global_cleanup();
        return false;
    }

        // 解析（兼容 valid 普通卡 + permanent 永久卡 + 简易格式）
    long long expire_ts = 0;
    string newsign;
    bool is_permanent = false;   // ← 加这一行

    
    if (reply.rfind("ok|", 0) == 0) {
        istringstream ss(reply);
        string p; int idx = 0; int server_minleft = 0; string type;
        while (getline(ss, p, '|')) {
            if (idx == 1) type = p;
            if (idx == 2 && type == "permanent") {
                try { expire_ts = stoll(p); } catch(...) {}
            }
            if (idx == 3 && type == "valid") {
                try { server_minleft = stoi(p); } catch(...) {}
            }
            if (idx == 4 && type == "valid") {
                try { expire_ts = stoll(p); } catch(...) {}
            }
            if (!type.empty() && idx == (type == "permanent" ? 3 : 5)) {
                size_t eq = p.find('=');
                if (eq != string::npos) newsign = p.substr(eq + 1);
            }
            idx++;
        }
        if (type == "permanent") is_permanent = true;
        if (server_minleft > 0) {
            expire_ts = (long long)time(nullptr) + server_minleft * 60LL;
        }
    } else {
        // 简易格式: 0|到期戳|sign=...
        istringstream ss(reply);
        string p; int idx = 0;
        while (getline(ss, p, '|')) {
            if (idx == 1) { try { expire_ts = stoll(p); } catch(...) { expire_ts = 0; } }
            if (idx == 2) { size_t eq = p.find('='); if (eq != string::npos) newsign = p.substr(eq + 1); }
            idx++;
        }
    }

    if (expire_ts == 0) { curl_global_cleanup(); return false; }

    int minleft;
    if (is_permanent) {
        minleft = 999999; // 永久卡，显示用
    } else {
        minleft = (int)((expire_ts - (long long)time(nullptr)) / 60);
    }
    if (minleft <= 0) { curl_global_cleanup(); return false; }

    if (!newsign.empty()) {
        // 确保 expire_ts 是基于本地时间算的（用 minleft 反推）
        long long local_expire = (long long)time(nullptr) + (long long)minleft * 60LL;
        SaveCache(newsign, local_expire);
    }

    
        is_permanent = (reply.find("|permanent|") != string::npos);

    cout << CYAN "╔══════════════════════════════════════╗" RESET "\n";
    cout << CYAN "║" RESET GREEN "  ✅ 验证成功！                       " CYAN "║" RESET "\n";
    if (is_permanent) {
        cout << CYAN "║" RESET "  ⏱  剩余: " YELLOW "永久" RESET "            " CYAN "║" RESET "\n";
    } else {
        int hours = minleft / 60, mins = minleft % 60;
        cout << CYAN "║" RESET "  ⏱  剩余: " YELLOW << hours << "小时" << mins << "分钟" << RESET
             << string(22 - to_string(hours).length() - to_string(mins).length(), ' ') << CYAN "║" RESET "\n";
    }
    cout << CYAN "║" RESET DIM "  正在进入工具...                     " CYAN "║" RESET "\n";
    cout << CYAN "╚══════════════════════════════════════╝" RESET "\n";

    cout.flush();
    this_thread::sleep_for(chrono::milliseconds(1200));

    if (is_permanent) {
    msg = "✅ 在线验证通过（永久授权）";
} else {
    int hours = minleft / 60, mins = minleft % 60;
    msg = "✅ 在线验证通过（剩余 " + to_string(hours) + "小时" + to_string(mins) + "分钟）";
}

    g_hb_card = card;
    g_hb_mac = mac;
    g_hb_running = true;
    thread(HeartbeatLoop).detach();
    return true;
}
