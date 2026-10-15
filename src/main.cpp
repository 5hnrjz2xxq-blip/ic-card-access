/*
 * IC 卡门禁系统 v0.3（NFC 卡内认证版，双读卡器）
 *
 * 读卡器：PN532（I2C）+ MFRC522（SPI）双读卡器同时工作
 * 认证方式：UID 白名单 + Ntag215 卡内密钥双重验证（防复制）
 *   - Ntag215（7字节UID）：UID 在白名单 且 卡内 Page 4-7 密钥匹配 → 通过
 *     （密钥读写经 PN532；MFRC522 读到 Ntag215 仅验 UID）
 *   - Mifare Classic（4字节UID）：仅 UID 白名单（兼容旧卡，双读卡器都支持）
 *
 * 密钥：16 字节，存于 Ntag215 Page 4-7。添加卡时自动写入（经 PN532）。
 *
 * 保留 v0.4 功能：OLED 显示、WiFi 热点 + Web 管理页、蜂鸣器、舵机、继电器
 * 管理卡：5B 50 E7 53（碰一下=添加模式，按住3秒=删除模式，双读卡器都识别）
 *
 * 接线：
 *   PN532: SDA=GPIO21, SCL=GPIO22（I2C，拨码 1→ON 2→OFF）
 *   MFRC522: SDA=GPIO5, RST=GPIO27, SCK=GPIO18, MISO=GPIO19, MOSI=GPIO23
 *   继电器:  IN=GPIO26（低电平触发）
 *   舵机:    信号=GPIO14
 *   蜂鸣器:  信号=GPIO13（有源，高电平触发）
 *   OLED:    SDA=GPIO21, SCL=GPIO22（I2C，与 PN532 共用总线）
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_PN532.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <U8g2lib.h>

// ─── 引脚 ────────────────────────────────────────────────────
#define PN532_IRQ   (-1)
#define PN532_RESET (-1)
#define I2C_SDA     21
#define I2C_SCL     22
#define MFRC522_SS_PIN   5
#define MFRC522_RST_PIN  27
#define RELAY_PIN   26
#define SERVO_PIN   14
#define BUZZER_PIN  13

// ─── 参数 ────────────────────────────────────────────────────
#define SERVO_OPEN_DEG    90
#define SERVO_CLOSE_DEG    0
#define DOOR_OPEN_MS    3000
#define MAX_CARDS         20
#define ADMIN_HOLD_MS   3000
#define ADMIN_TIMEOUT_MS  5000
#define GONE_DEBOUNCE_MS  200
#define WEB_LEARN_MS     15000
#define SECRET_PAGES       4  // 密钥起始页
#define SECRET_LEN        16  // 密钥长度（4页 x 4字节）

// ─── 卡内密钥（16字节，写入 Ntag215 Page 4-7） ───────────────
// ⚠️ 安全提示：请替换为你自己的 16 字节密钥，切勿将真实密钥提交到公开仓库。
// 部署前先用管理卡/网页录入把密钥写入你的 Ntag215 卡。
const uint8_t NFC_SECRET[SECRET_LEN] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// ─── WiFi 热点 ───────────────────────────────────────────────
const char* AP_SSID = "门禁管理";
const char* AP_PASS = "YOUR_AP_PASSWORD";  // ← 改成你自己的热点密码
WebServer server(80);

// ─── OLED ────────────────────────────────────────────────────
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ─── 管理卡 UID ──────────────────────────────────────────────
const uint8_t MASTER_UID[4] = {0x5B, 0x50, 0xE7, 0x53};

// ─── 默认白名单 ───────────────────────────────────────────────
const char* DEFAULT_CARDS[] = {
    "3320F505",
    "D30C8B8E",
};
const uint8_t DEFAULT_CARD_COUNT = sizeof(DEFAULT_CARDS) / sizeof(DEFAULT_CARDS[0]);

// ─── 状态机 ──────────────────────────────────────────────────
enum class State : uint8_t { IDLE, PENDING, ADD_MODE, DELETE_MODE };
State    state          = State::IDLE;
uint32_t pendingStartMs = 0;
uint32_t lastPresentMs  = 0;
uint32_t modeEnterMs    = 0;
bool     holdBeepDone   = false;
bool     waitMasterGone = false;
uint32_t masterGoneMs   = 0;

bool     webLearnMode  = false;
uint32_t webLearnStart = 0;

// ─── 全局对象 ────────────────────────────────────────────────
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);
MFRC522 mfrc522(MFRC522_SS_PIN, MFRC522_RST_PIN);
Servo      doorServo;
Preferences prefs;

// 读卡器标识
enum class Reader : uint8_t { NONE, PN532, MFRC522 };
Reader lastReader = Reader::NONE;

// 防重复提示：记住上次被拒绝的 UID，2 秒内不再刷屏
String lastRejectUid = "";
uint32_t lastRejectMs = 0;
bool shouldSuppressReject(const String& uid) {
    uint32_t now = millis();
    if (uid == lastRejectUid && now - lastRejectMs < 2000) return true;
    lastRejectUid = uid; lastRejectMs = now;
    return false;
}

// ─── 蜂鸣器 ─────────────────────────────────────────────────
void beepOk()   { digitalWrite(BUZZER_PIN, HIGH); delay(150); digitalWrite(BUZZER_PIN, LOW); }
void beepDeny() { for (int i = 0; i < 2; i++) { digitalWrite(BUZZER_PIN, HIGH); delay(100); digitalWrite(BUZZER_PIN, LOW); delay(100); } }
void beepEnterAdd() { digitalWrite(BUZZER_PIN, HIGH); delay(150); digitalWrite(BUZZER_PIN, LOW); }
void beepEnterDel() { for (int i = 0; i < 2; i++) { digitalWrite(BUZZER_PIN, HIGH); delay(150); digitalWrite(BUZZER_PIN, LOW); delay(100); } }
void beepExit() { for (int i = 0; i < 2; i++) { digitalWrite(BUZZER_PIN, HIGH); delay(80); digitalWrite(BUZZER_PIN, LOW); delay(80); } }

// ─── UID 工具 ─────────────────────────────────────────────────
String uidToStr(uint8_t *uid, uint8_t size) {
    String s = "";
    for (uint8_t i = 0; i < size; i++) { if (uid[i] < 0x10) s += "0"; s += String(uid[i], HEX); }
    s.toUpperCase(); return s;
}
void printUID(uint8_t *uid, uint8_t size) {
    for (uint8_t i = 0; i < size; i++) { if (uid[i] < 0x10) Serial.print("0"); Serial.print(uid[i], HEX); if (i < size - 1) Serial.print(" "); }
}
bool uidIsAdmin(uint8_t *uid, uint8_t size) {
    if (size != 4) return false;
    return memcmp(uid, MASTER_UID, 4) == 0;
}
bool isNtag215(uint8_t size) { return size == 7; }

// ─── Ntag215 密钥读写（带重试，贴纸卡天线小、位置敏感） ─────────
bool readSecret(uint8_t *out) {
    for (uint8_t retry = 0; retry < 3; retry++) {
        bool ok = true;
        for (uint8_t p = 0; p < SECRET_PAGES; p++) {
            if (!nfc.ntag2xx_ReadPage(SECRET_PAGES + p, out + p * 4)) { ok = false; break; }
        }
        if (ok) return true;
        delay(20);
    }
    return false;
}
bool writeSecret() {
    for (uint8_t retry = 0; retry < 3; retry++) {
        bool ok = true;
        for (uint8_t p = 0; p < SECRET_PAGES; p++) {
            if (!nfc.ntag2xx_WritePage(SECRET_PAGES + p, (uint8_t*)(NFC_SECRET + p * 4))) { ok = false; break; }
        }
        if (ok) {
            // 写完读回校验
            uint8_t verify[SECRET_LEN];
            if (readSecret(verify) && memcmp(verify, NFC_SECRET, SECRET_LEN) == 0) return true;
        }
        delay(20);
    }
    return false;
}
bool verifySecret() {
    uint8_t buf[SECRET_LEN];
    if (!readSecret(buf)) return false;
    return memcmp(buf, NFC_SECRET, SECRET_LEN) == 0;
}

// ─── NVS 白名单 ───────────────────────────────────────────────
int getCardCount() { return prefs.getInt("count", 0); }
String getCard(int idx) { char k[8]; snprintf(k, sizeof(k), "c%d", idx); return prefs.getString(k, ""); }
bool isAuthorized(const String& uid) {
    int cnt = getCardCount();
    for (int i = 0; i < cnt; i++) if (getCard(i).equalsIgnoreCase(uid)) return true;
    return false;
}
bool addCard(const String& uid) {
    int cnt = getCardCount();
    if (cnt >= MAX_CARDS) return false;
    for (int i = 0; i < cnt; i++) if (getCard(i).equalsIgnoreCase(uid)) return false;
    char k[8]; snprintf(k, sizeof(k), "c%d", cnt);
    prefs.putString(k, uid); prefs.putInt("count", cnt + 1); return true;
}
bool removeCard(const String& uid) {
    int cnt = getCardCount(), found = -1;
    for (int i = 0; i < cnt; i++) if (getCard(i).equalsIgnoreCase(uid)) { found = i; break; }
    if (found < 0) return false;
    char kf[8], kl[8]; snprintf(kf, sizeof(kf), "c%d", found); snprintf(kl, sizeof(kl), "c%d", cnt - 1);
    if (found != cnt - 1) prefs.putString(kf, getCard(cnt - 1));
    prefs.remove(kl); prefs.putInt("count", cnt - 1); return true;
}
void printWhitelist() {
    int cnt = getCardCount();
    Serial.printf("[白名单] 共 %d 张:\n", cnt);
    for (int i = 0; i < cnt; i++) Serial.printf("  [%d] %s\n", i + 1, getCard(i).c_str());
}
void initDefaultCards() {
    if (getCardCount() == 0) {
        for (uint8_t i = 0; i < DEFAULT_CARD_COUNT; i++) addCard(String(DEFAULT_CARDS[i]));
        Serial.println("[NVS] 写入默认白名单");
    }
}

// ─── OLED 显示 ────────────────────────────────────────────────
void oledShow(const char* l1, const char* l2) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    u8g2.drawUTF8(0, 16, l1);
    if (l2) u8g2.drawUTF8(0, 36, l2);
    char ip[24]; snprintf(ip, sizeof(ip), "IP:%s", WiFi.softAPIP().toString().c_str());
    u8g2.drawUTF8(0, 52, ip);
    char cnt[24]; snprintf(cnt, sizeof(cnt), "白名单:%d张", getCardCount());
    u8g2.drawUTF8(0, 64, cnt);
    u8g2.sendBuffer();
}
void oledSimple(const char* l1, const char* l2) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_wqy16_t_gb2312);
    u8g2.drawUTF8(0, 28, l1);
    if (l2) u8g2.drawUTF8(0, 52, l2);
    u8g2.sendBuffer();
}
void oledIdle()    { oledSimple("就绪，请刷卡", nullptr); }
void oledOk()      { oledSimple("欢迎", nullptr); }
void oledDeny()    { oledSimple("错误", nullptr); }
void oledAddMode() { oledShow("添加模式", "刷用户卡录入"); }
void oledDelMode() { oledShow("删除模式", "刷用户卡删除"); }

// ─── 开门 ────────────────────────────────────────────────────
void openDoor() {
    Serial.println("[门禁] 开门...");
    digitalWrite(RELAY_PIN, LOW);
    doorServo.write(SERVO_OPEN_DEG);
    delay(DOOR_OPEN_MS);
    doorServo.write(SERVO_CLOSE_DEG);
    digitalWrite(RELAY_PIN, HIGH);
    Serial.println("[门禁] 已关门");
    oledIdle();
}

void exitAdminMode(const char* reason) {
    beepExit(); state = State::IDLE; waitMasterGone = false;
    Serial.printf("[管理] 退出（%s）\n", reason); printWhitelist(); oledIdle();
}

// 读一张卡（522 优先：Classic 直接处理；Ntag215 转交 PN532 做密钥验证）
// MFRC522 统一用 WUPA：能检测到已 Halt 的卡；读完必 Halt，保证状态机干净
bool readOneCard(uint8_t *uid, uint8_t *uidLen, uint16_t timeoutMs) {
    // 1. MFRC522 先试（快）
    byte atqa[2]; byte sz = sizeof(atqa);
    if (mfrc522.PICC_WakeupA(atqa, &sz) == MFRC522::STATUS_OK &&
        mfrc522.PICC_ReadCardSerial()) {
        *uidLen = mfrc522.uid.size;
        memcpy(uid, mfrc522.uid.uidByte, *uidLen);
        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();

        if (*uidLen == 7) {
            // Ntag215：转交 PN532。先关 522 天线让卡掉电复位，再用 532 重读
            mfrc522.PCD_AntennaOff();
            delay(30);  // 等卡掉电复位
            bool ok = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, uidLen, timeoutMs);
            mfrc522.PCD_AntennaOn();
            if (ok) { lastReader = Reader::PN532; return true; }
            // 532 也没读到：回退用 522 读到的 UID，上层提示换读卡器
            lastReader = Reader::MFRC522;
            return true;
        }
        // Classic 卡：522 直接处理
        lastReader = Reader::MFRC522;
        return true;
    }

    // 2. 522 没读到，试 PN532（卡可能直接放在 532 上）
    mfrc522.PCD_AntennaOff();
    bool ok = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, uidLen, timeoutMs);
    mfrc522.PCD_AntennaOn();
    if (ok) { lastReader = Reader::PN532; return true; }

    lastReader = Reader::NONE;
    return false;
}

// PN532 释放卡片并复位状态（每次处理完调用，防止一次性问题）
void pn532Release() {
    if (lastReader == Reader::PN532) {
        nfc.SAMConfig();  // 复位 PN532 状态机，清除已选中的卡
        delay(10);
    }
}

// ─── Web 页面 ─────────────────────────────────────────────────
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>门禁白名单管理</title>
<style>body{font-family:sans-serif;max-width:480px;margin:20px auto;padding:0 16px}
.card{border:1px solid #ddd;border-radius:8px;padding:12px;margin:8px 0}
button{padding:8px 16px;margin:4px;border-radius:6px;border:1px solid #999;background:#f5f5f5}
button.danger{background:#fdd;border-color:#c66}input{padding:8px;width:60%}</style>
</head><body>
<h2>门禁白名单管理 v0.3</h2>
<div class='card'><b>白名单（<span id='cnt'>?</span> 张）</b><div id='list'>加载中…</div></div>
<div class='card'><b>添加卡片</b><br>
<input id='uid' placeholder='UID，如 3320F505'>
<button onclick='addUid()'>手动添加</button><br>
<button onclick='learn()'>刷卡录入（15秒内刷卡，Ntag215自动写密钥）</button>
<span id='learnMsg'></span></div>
<div class='card'><button class='danger' onclick='if(confirm("清空并恢复默认？"))fetch("/clear",{method:"POST"}).then(refresh)'>清空并恢复默认</button></div>
<script>
function refresh(){fetch('/api/list').then(r=>r.json()).then(d=>{
 document.getElementById('cnt').innerText=d.count;
 let h='';d.cards.forEach((c,i)=>{h+='<div>'+(i+1)+'. '+c+' <button class="danger" onclick="del(\''+c+'\')">删除</button></div>'});
 document.getElementById('list').innerHTML=h||'（空）';});}
function del(uid){fetch('/del?uid='+uid,{method:'POST'}).then(refresh);}
function addUid(){let u=document.getElementById('uid').value.trim().toUpperCase();
 if(!u)return;fetch('/add?uid='+u,{method:'POST'}).then(r=>r.text()).then(t=>{alert(t);refresh();});}
function learn(){document.getElementById('learnMsg').innerText='请在15秒内刷卡…';
 fetch('/learn',{method:'POST'}).then(r=>r.text()).then(t=>{document.getElementById('learnMsg').innerText=t;refresh();});}
refresh();</script>
</body></html>)rawliteral";

void handleRoot()   { server.send_P(200, "text/html", HTML_PAGE); }
void handleApiList() {
    int cnt = getCardCount();
    String j = "{\"count\":" + String(cnt) + ",\"cards\":[";
    for (int i = 0; i < cnt; i++) { if (i) j += ","; j += "\"" + getCard(i) + "\""; }
    j += "]}";
    server.send(200, "application/json", j);
}
void handleAdd() {
    String uid = server.arg("uid"); uid.trim(); uid.toUpperCase();
    if (uid.length() == 0) { server.send(400, "text/plain", "UID为空"); return; }
    if (addCard(uid)) { server.send(200, "text/plain", "已添加: " + uid); oledShow("Web添加", uid.c_str()); }
    else server.send(200, "text/plain", "已存在或已满: " + uid);
}
void handleDel() {
    String uid = server.arg("uid");
    if (removeCard(uid)) server.send(200, "text/plain", "OK");
    else server.send(404, "text/plain", "不在白名单");
}
void handleClear() {
    prefs.clear(); initDefaultCards();
    server.send(200, "text/plain", "已重置"); oledIdle();
}
void handleLearn() {
    oledShow("Web录入", "15秒内刷卡");
    Serial.println("[Web] 进入刷卡录入模式，15秒（Ntag215自动写密钥）");
    String result = "超时，未检测到卡片";
    uint32_t t0 = millis();
    uint8_t uid[7] = {0}; uint8_t uidLen = 0;
    while (millis() - t0 < WEB_LEARN_MS) {
        server.handleClient();
        if (readOneCard(uid, &uidLen, 50)) {
            String uidStr = uidToStr(uid, uidLen);
            // Ntag215 自动写密钥（必须经 PN532）
            if (isNtag215(uidLen)) {
                if (lastReader != Reader::PN532) {
                    result = "Ntag215 请放到 PN532 上: " + uidStr; beepDeny();
                    Serial.println("[Web] " + result);
                    pn532Release();
                    break;
                }
                if (!writeSecret()) {
                    result = "密钥写入失败: " + uidStr; beepDeny();
                    Serial.println("[Web] " + result);
                    pn532Release();
                    break;
                }
                Serial.println("[Web] Ntag215 密钥已写入");
            }
            if (addCard(uidStr)) { result = "已录入: " + uidStr; beepOk(); oledShow("Web录入成功", uidStr.c_str()); }
            else { result = "已存在或已满: " + uidStr; beepDeny(); }
            Serial.println("[Web] " + result);
            pn532Release();
            break;
        }
        delay(50);
    }
    oledIdle();
    server.send(200, "text/plain", result);
}

// ─── 串口指令 ────────────────────────────────────────────────
void processSerial() {
    if (!Serial.available()) return;
    String cmd = Serial.readStringUntil('\n'); cmd.trim();
    if (cmd.equalsIgnoreCase("LIST")) printWhitelist();
    else if (cmd.equalsIgnoreCase("CLEAR")) { prefs.clear(); initDefaultCards(); Serial.println("[串口] 已重置"); oledIdle(); }
    else if (cmd.length() > 0) Serial.println("[串口] 指令：LIST | CLEAR");
}

// ─── setup ───────────────────────────────────────────────────
void setup() {
    Serial.begin(115200); delay(500);
    Serial.println("\n=== IC 卡门禁 v0.3 NFC 双读卡器 ===");

    pinMode(RELAY_PIN, OUTPUT);  digitalWrite(RELAY_PIN, HIGH);
    pinMode(BUZZER_PIN, OUTPUT); digitalWrite(BUZZER_PIN, LOW);
    doorServo.attach(SERVO_PIN); doorServo.write(SERVO_CLOSE_DEG);

    u8g2.begin(); u8g2.enableUTF8Print();
    oledSimple("启动中...", nullptr);

    Wire.begin(I2C_SDA, I2C_SCL);
    nfc.begin();
    uint32_t ver = nfc.getFirmwareVersion();
    if (!ver) {
        Serial.println("[错误] PN532 未找到！检查拨码和接线");
        oledSimple("PN532 错误", "检查接线拨码");
        while (1) delay(100);
    }
    nfc.SAMConfig();
    Serial.printf("[PN532] 就绪，固件 v%d.%d\n", (ver >> 16) & 0xFF, (ver >> 8) & 0xFF);

    SPI.begin();
    mfrc522.PCD_Init();
    Serial.print("[MFRC522] 就绪，固件版本: ");
    mfrc522.PCD_DumpVersionToSerial();

    prefs.begin("acl", false);
    initDefaultCards(); printWhitelist();

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("[WiFi] 热点 %s 已启动，IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());

    server.on("/", handleRoot);
    server.on("/api/list", handleApiList);
    server.on("/add", HTTP_POST, handleAdd);
    server.on("/del", HTTP_POST, handleDel);
    server.on("/clear", HTTP_POST, handleClear);
    server.on("/learn", HTTP_POST, handleLearn);
    server.begin();
    Serial.println("[Web] 管理页 http://192.168.4.1");

    oledIdle();
    Serial.println("[系统] 就绪，请刷卡...");
}

// ─── loop ────────────────────────────────────────────────────
void loop() {
    server.handleClient();
    processSerial();
    uint32_t now = millis();

    // 管理卡按压时长判断（PENDING 状态）
    if (state == State::PENDING) {
        uint8_t uid[7] = {0}; uint8_t uidLen = 0;
        bool found = readOneCard(uid, &uidLen, 50) && uidIsAdmin(uid, uidLen);
        if (found) lastPresentMs = now;
        bool cardGone = !found && (now - lastPresentMs > GONE_DEBOUNCE_MS);
        if (!holdBeepDone && now - pendingStartMs >= ADMIN_HOLD_MS) {
            holdBeepDone = true; state = State::DELETE_MODE; modeEnterMs = now; waitMasterGone = true;
            beepEnterDel(); oledDelMode();
            Serial.println("[管理] 进入【删除模式】，等管理卡离场...");
            return;
        }
        if (cardGone && !holdBeepDone) {
            state = State::ADD_MODE; modeEnterMs = now;
            beepEnterAdd(); oledAddMode();
            Serial.println("[管理] 进入【添加模式】");
        }
        return;
    }

    if ((state == State::ADD_MODE || state == State::DELETE_MODE) && now - modeEnterMs >= ADMIN_TIMEOUT_MS) {
        exitAdminMode("超时"); return;
    }

    // 读卡（非阻塞短超时）
    uint8_t uid[7] = {0};
    uint8_t uidLen = 0;
    if (!readOneCard(uid, &uidLen, 50)) {
        if (state == State::DELETE_MODE && waitMasterGone) {
            waitMasterGone = false;
            masterGoneMs = now;
            Serial.println("[管理] 管理卡已离场，现在刷用户卡删除");
        }
        return;
    }

    String uidStr = uidToStr(uid, uidLen);
    Serial.print("[RFID] UID: "); printUID(uid, uidLen); Serial.println();

    // 管理卡
    if (uidIsAdmin(uid, uidLen)) {
        if (state == State::IDLE) {
            state = State::PENDING; pendingStartMs = now; lastPresentMs = now; holdBeepDone = false;
            Serial.println("[管理] 检测到管理卡，判断停留时长...");
            return;
        }
        if (state == State::DELETE_MODE && waitMasterGone) return;
        if (state == State::ADD_MODE) {
            exitAdminMode("手动退出"); return;
        }
        if (state == State::DELETE_MODE) {
            if (now - masterGoneMs < 2000) return;  // 防抖
            exitAdminMode("手动退出"); return;
        }
    }

    // 添加模式：Ntag215 写密钥 + 入白名单
    if (state == State::ADD_MODE) {
        if (isNtag215(uidLen)) {
            if (lastReader != Reader::PN532) {
                if (!shouldSuppressReject(uidStr)) {
                    beepDeny(); oledShow("请用PN532", "添加Ntag215");
                    Serial.printf("[添加] Ntag215 请放到 PN532 上: %s\n", uidStr.c_str());
                }
                modeEnterMs = now; pn532Release(); return;
            }
            if (!writeSecret()) {
                beepDeny(); oledShow("密钥写入失败", uidStr.c_str());
                Serial.printf("[添加] 密钥写入失败: %s\n", uidStr.c_str());
                modeEnterMs = now; pn532Release(); return;
            }
            Serial.printf("[添加] Ntag215 密钥已写入: %s\n", uidStr.c_str());
        }
        if (addCard(uidStr)) { beepOk(); oledShow("已添加", uidStr.c_str()); Serial.printf("[添加] 已添加: %s\n", uidStr.c_str()); }
        else { beepDeny(); Serial.printf("[添加] 已存在或已满: %s\n", uidStr.c_str()); }
        modeEnterMs = now; pn532Release(); return;
    }

    // 删除模式
    if (state == State::DELETE_MODE) {
        if (waitMasterGone) return;
        if (removeCard(uidStr)) { beepDeny(); oledShow("已删除", uidStr.c_str()); Serial.printf("[删除] 已删除: %s\n", uidStr.c_str()); }
        else { beepOk(); Serial.printf("[删除] 不在白名单: %s\n", uidStr.c_str()); }
        modeEnterMs = now; pn532Release(); return;
    }

    // 普通刷卡：UID 白名单 + Ntag215 密钥双重验证
    if (isAuthorized(uidStr)) {
        if (isNtag215(uidLen)) {
            // Ntag215 必须经 PN532 验密钥（MFRC522 上只认 UID，不安全）
            if (lastReader != Reader::PN532) {
                if (!shouldSuppressReject(uidStr)) {
                    Serial.println("[门禁] ✘ 拒绝（Ntag215 请用 PN532 读卡器）");
                    beepDeny(); oledDeny();
                    delay(800); oledIdle();
                }
                pn532Release(); return;
            }
            if (!verifySecret()) {
                Serial.println("[门禁] ✘ 拒绝（密钥不匹配，疑似复制卡）");
                beepDeny(); oledDeny();
                delay(1500); oledIdle();
                pn532Release(); return;
            }
            Serial.println("[门禁] ✔ 密钥验证通过");
        }
        Serial.println("[门禁] ✔ 授权通过"); beepOk(); oledOk();
        pn532Release();
        openDoor();
    } else {
        Serial.println("[门禁] ✘ 拒绝"); beepDeny(); oledDeny();
        delay(1500); oledIdle();
        pn532Release();
    }
}
