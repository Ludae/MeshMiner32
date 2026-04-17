 //*
 * ═══════════════════════════════════════════════════════════════
 *  MeshMiner32 Edition  —  Single-file Arduino sketch
 *  Target  : ESP32 DevKit V1  (esp32 board package 3.x)
 *  Pool    : public-pool.io:3333  (0% fee, verify at web.public-pool.io)
 *  Display : SH1106 128x64 — 4-wire Hardware SPI
 *
 *  Libraries (install via Arduino Library Manager):
 *    ArduinoJson  >= 7.0   (bblanchon)
 *    U8g2         >= 2.35  (olikraus)
 *
 *  SPI OLED wiring (ESP32 DevKit V1):
 *  ┌─────────────┬───────────────────────────────┐
 *  │  OLED pin   │  ESP32 pin                    │
 *  ├─────────────┼───────────────────────────────┤
 *  │  VCC        │  3.3V                         │
 *  │  GND        │  GND                          │
 *  │  CLK / D0   │  GPIO 18  (HW SPI SCK)        │
 *  │  MOSI / D1  │  GPIO 23  (HW SPI MOSI)       │
 *  │  CS         │  GND  (CS tied low on module) │
 *  │  DC         │  GPIO 22                      │
 *  │  RST        │  GPIO  4                      │
 *  └─────────────┴───────────────────────────────┘
 *
 *  Monitor your miner at: https://web.public-pool.io
 *  Enter your BTC address to see hashrate & shares live.
 * ═══════════════════════════════════════════════════════════════
 */

// ───────────────────────────────────────────────────────────────
//  SECTION 1 — USER CONFIGURATION  (edit these before flashing)
// ───────────────────────────────────────────────────────────────

#define WIFI_SSID        "Home Assistant"
#define WIFI_PASSWORD    "7537819ajk"
#define WIFI_TIMEOUT_MS  20000

// public-pool.io — 0% fee solo pool, tracks workers by BTC address
// Append a worker name after a dot so it shows in the dashboard
// e.g.  bc1qXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX.esp32master
#define POOL_HOST   "public-pool.io"
#define POOL_PORT   3333
#define POOL_USER   "bc1qdh02k3mrznn038g62arfpe8n42nk9uc96hcpty.MeshMiner32"
#define POOL_PASS   "x"

#define STRATUM_TIMEOUT_MS  10000

// Node role:
//   ROLE_AUTO   = tries WiFi → if connected becomes master, else worker
//   ROLE_MASTER = always master (needs WiFi creds above)
//   ROLE_WORKER = always worker  (no WiFi needed)
#define ROLE_AUTO    0
#define ROLE_MASTER  1
#define ROLE_WORKER  2
#define NODE_ROLE    ROLE_AUTO

// ── SH1106 SPI pins ──────────────────────────────────────────
//  SCL (CLK)  -> GPIO 18  (ESP32 HW SPI SCK,  label D18)
//  SDA (MOSI) -> GPIO 23  (ESP32 HW SPI MOSI, label D23)
//  DC         -> GPIO 22  (label D22)
//  RES        -> GPIO  4  (label D4)
//  CS         -> GND on module — use U8X8_PIN_NONE in code
#define OLED_DC   22
#define OLED_RST  4

#define OLED_REFRESH_MS   1000
#define PAGE_FLIP_MS      3000   // auto-advance page every N ms
#define DISPLAY_SLEEP_MS  30000  // turn off OLED after 30s of inactivity (0 = never)
#define BOOT_BTN_PIN        0    // GPIO0 = BOOT button on DevKit V1

// ── Mining task ──────────────────────────────────────────────
#define MINING_TASK_PRIORITY  5
#define MINING_TASK_CORE      1      // core 1 — WiFi stays on core 0
#define MINING_TASK_STACK     12288

// ── ESP-NOW mesh ─────────────────────────────────────────────
#define ESPNOW_CHANNEL          1
#define PEER_TIMEOUT_MS         15000
#define HEARTBEAT_INTERVAL_MS   5000
#define ELECTION_TRIGGER_COUNT  3
#define ELECTION_BACKOFF_MAX_MS 2000
#define MESH_MAX_PEERS          20
#define MESH_FRAME_LEN          250

#define SERIAL_BAUD       115200
#define LED_PIN           2    // Built-in blue LED on ESP32 DevKit V1
#define POOL_RETRY_MS  15000   // Only retry pool connection every 15s (non-blocking)

// ───────────────────────────────────────────────────────────────
//  SECTION 2 — INCLUDES
// ───────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <functional>
#include <string.h>

// ───────────────────────────────────────────────────────────────
//  SECTION 3 — FORWARD DECLARATIONS
//  (prevents Arduino IDE preprocessor ordering issues)
// ───────────────────────────────────────────────────────────────

struct StratumJob;
struct StratumSession;
struct MinerJob;
struct MinerStats;
struct PeerInfo;
struct DisplayData;

static void dispatchJob(const StratumJob& sj);
static void checkElection();

// ───────────────────────────────────────────────────────────────
//  SECTION 4 — SHA-256  (double SHA for Bitcoin mining)
// ───────────────────────────────────────────────────────────────

static const uint32_t SHA_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static const uint32_t SHA_H0[8] = {
    0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
};

#define ROTR32(x,n)    (((x)>>(n))|((x)<<(32-(n))))
#define SHA_CH(e,f,g)  (((e)&(f))^(~(e)&(g)))
#define SHA_MAJ(a,b,c) (((a)&(b))^((a)&(c))^((b)&(c)))
#define SHA_EP0(a)     (ROTR32(a,2) ^ROTR32(a,13)^ROTR32(a,22))
#define SHA_EP1(e)     (ROTR32(e,6) ^ROTR32(e,11)^ROTR32(e,25))
#define SHA_SIG0(x)    (ROTR32(x,7) ^ROTR32(x,18)^((x)>>3))
#define SHA_SIG1(x)    (ROTR32(x,17)^ROTR32(x,19)^((x)>>10))

static inline uint32_t sha_be32(const uint8_t* p){
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static inline void sha_wr32(uint8_t* p,uint32_t v){
    p[0]=(v>>24)&0xFF;p[1]=(v>>16)&0xFF;p[2]=(v>>8)&0xFF;p[3]=v&0xFF;
}

static void sha256_compress(const uint32_t* in16,uint32_t* st){
    uint32_t w[64];
    for(int i=0;i<16;i++) w[i]=in16[i];
    for(int i=16;i<64;i++) w[i]=SHA_SIG1(w[i-2])+w[i-7]+SHA_SIG0(w[i-15])+w[i-16];
    uint32_t a=st[0],b=st[1],c=st[2],d=st[3],e=st[4],f=st[5],g=st[6],h=st[7];
    for(int i=0;i<64;i++){
        uint32_t t1=h+SHA_EP1(e)+SHA_CH(e,f,g)+SHA_K[i]+w[i];
        uint32_t t2=SHA_EP0(a)+SHA_MAJ(a,b,c);
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    st[0]+=a;st[1]+=b;st[2]+=c;st[3]+=d;
    st[4]+=e;st[5]+=f;st[6]+=g;st[7]+=h;
}

static void double_sha256(const uint8_t* data,size_t len,uint8_t* digest){
    uint8_t mid[32];
    {
        uint32_t st[8]; memcpy(st,SHA_H0,32);
        uint8_t buf[64];
        size_t rem=len; const uint8_t* ptr=data;
        while(rem>=64){
            uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(ptr+i*4);
            sha256_compress(w,st); ptr+=64; rem-=64;
        }
        memset(buf,0,64); memcpy(buf,ptr,rem); buf[rem]=0x80;
        if(rem>=56){
            uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(buf+i*4);
            sha256_compress(w,st); memset(buf,0,64);
        }
        uint64_t bits=(uint64_t)len*8;
        for(int i=0;i<8;i++) buf[56+i]=(bits>>((7-i)*8))&0xFF;
        uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(buf+i*4);
        sha256_compress(w,st);
        for(int i=0;i<8;i++) sha_wr32(mid+i*4,st[i]);
    }
    {
        uint32_t st[8]; memcpy(st,SHA_H0,32);
        uint8_t buf[64]={0}; memcpy(buf,mid,32); buf[32]=0x80;
        buf[62]=0x01; buf[63]=0x00;
        uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(buf+i*4);
        sha256_compress(w,st);
        for(int i=0;i<8;i++) sha_wr32(digest+i*4,st[i]);
    }
}

static void bitcoin_hash(const uint8_t* h80,uint8_t* o32){ double_sha256(h80,80,o32); }

static bool check_difficulty(const uint8_t* hash,const uint8_t* tgt){
    for(int i=31;i>=0;i--){
        if(hash[i]<tgt[i]) return true;
        if(hash[i]>tgt[i]) return false;
    }
    return true;
}

static void nbits_to_target(const uint8_t* nb,uint8_t* t32){
    memset(t32,0,32); uint8_t exp=nb[0]; if(exp==0||exp>32) return;
    int pos=(int)exp-1;
    if(pos>=0   &&pos<32) t32[pos]  =nb[1];
    if(pos-1>=0 &&pos-1<32) t32[pos-1]=nb[2];
    if(pos-2>=0 &&pos-2<32) t32[pos-2]=nb[3];
}

// Convert stratum pool difficulty to a share target (index 31 = MSB).
// diff1 target = 0x00000000FFFF0000...0000  (0xFF at byte 27, 0xFF at byte 26)
// target_D = diff1_target / D  (long division, byte by byte from MSB)
static void difficulty_to_target(uint32_t diff, uint8_t* t32){
    memset(t32,0,32);
    if(diff==0) diff=1;
    uint64_t rem=0;
    for(int i=31;i>=0;i--){
        uint64_t cur=rem*256;
        if(i==27) cur+=0xFF;
        else if(i==26) cur+=0xFF;
        t32[i]=(uint8_t)(cur/diff);
        rem=cur%diff;
    }
}

// ───────────────────────────────────────────────────────────────
//  SECTION 5 — HEX HELPERS
// ───────────────────────────────────────────────────────────────

static void hexToBytes(const char* hex,uint8_t* out,int maxLen){
    int len=(int)(strlen(hex)/2); if(len>maxLen) len=maxLen;
    for(int i=0;i<len;i++){
        auto nib=[](char c)->uint8_t{
            if(c>='0'&&c<='9') return c-'0';
            if(c>='a'&&c<='f') return c-'a'+10;
            if(c>='A'&&c<='F') return c-'A'+10;
            return 0;
        };
        out[i]=(nib(hex[i*2])<<4)|nib(hex[i*2+1]);
    }
}

static String bytesToHex(const uint8_t* b,int len){
    static const char* H="0123456789abcdef";
    String s; s.reserve(len*2);
    for(int i=0;i<len;i++){s+=H[(b[i]>>4)&0xF];s+=H[b[i]&0xF];}
    return s;
}

// ───────────────────────────────────────────────────────────────
//  SECTION 6 — STRATUM STRUCTS
// ───────────────────────────────────────────────────────────────

struct StratumJob {
    char     job_id[64];
    uint8_t  prev_hash[32];
    uint8_t  coinbase1[128]; uint16_t coinbase1_len;
    uint8_t  coinbase2[128]; uint16_t coinbase2_len;
    uint8_t  merkle_branches[16][32];
    uint8_t  merkle_count;
    uint8_t  version[4];
    uint8_t  nbits[4];
    uint8_t  ntime[4];
    bool     clean_jobs;
    bool     valid;
};

struct StratumSession {
    uint8_t  extranonce1[8];
    uint8_t  extranonce1_len;
    uint8_t  extranonce2_len;
    uint32_t difficulty;
    bool     subscribed;
    bool     authorized;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 7 — ESP-NOW MESH STRUCTS
// ───────────────────────────────────────────────────────────────

#define MSG_JOB        0x01
#define MSG_RESULT     0x02
#define MSG_STATS      0x03
#define MSG_HEARTBEAT  0x04
#define MSG_ELECT      0x05

static const uint8_t ESPNOW_BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

#pragma pack(push,1)
typedef struct {
    uint8_t  msg_type;
    uint8_t  job_id;
    uint8_t  version[4];
    uint8_t  prev_hash[32];
    uint8_t  merkle_root[32];
    uint8_t  nbits[4];
    uint8_t  ntime[4];
    uint32_t nonce_start;
    uint32_t nonce_end;
    uint8_t  extranonce2[8];
    uint8_t  extranonce2_len;
    uint8_t  assigned_chunk;
    uint32_t pool_difficulty;   // stratum share difficulty — used for target, NOT nbits
} MeshJobMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  job_id;
    uint32_t nonce;
    uint8_t  worker_mac[6];
} MeshResultMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  worker_mac[6];
    uint32_t hash_rate;
    uint32_t nonces_tested;
    uint8_t  job_id;
} MeshStatsMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  sender_mac[6];
    uint8_t  is_master;
    uint32_t uptime_s;
    uint32_t total_hash_rate;
} MeshHeartbeatMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  candidate_mac[6];
    uint8_t  priority;
} MeshElectMsg;
#pragma pack(pop)

struct PeerInfo {
    uint8_t  mac[6];
    bool     active;
    uint32_t last_seen_ms;
    uint32_t hash_rate;
    uint8_t  assigned_chunk;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 8 — EspNowMesh CLASS
// ───────────────────────────────────────────────────────────────

class EspNowMesh {
public:
    static EspNowMesh& instance(){ static EspNowMesh i; return i; }

    bool begin(bool isMaster, uint8_t channel=ESPNOW_CHANNEL){
        _isMaster=isMaster;
        _channel=channel;
        if(isMaster) WiFi.mode(WIFI_AP_STA);
        else       { WiFi.mode(WIFI_STA); WiFi.disconnect(); }
        // Force ESP-NOW onto the same channel WiFi is using
        esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
        Serial.printf("[Mesh] ESP-NOW channel: %d\n",(int)_channel);
        if(esp_now_init()!=ESP_OK){ Serial.println("[Mesh] init failed"); return false; }
        esp_now_register_recv_cb(_recv_cb);
        esp_now_register_send_cb(_send_cb);
        esp_now_peer_info_t bc={};
        memcpy(bc.peer_addr,ESPNOW_BROADCAST,6);
        bc.channel=_channel; bc.encrypt=false;
        if(esp_now_add_peer(&bc)!=ESP_OK){ Serial.println("[Mesh] bcast fail"); return false; }
        _initialized=true;
        Serial.printf("[Mesh] ready as %s\n",isMaster?"MASTER":"WORKER");
        return true;
    }

    bool broadcastJob(const MeshJobMsg& j)                  { return _tx(ESPNOW_BROADCAST,&j,sizeof(j)); }
    bool sendResult(const uint8_t* m,const MeshResultMsg& r){ return _tx(m,&r,sizeof(r)); }
    bool sendStats(const uint8_t* m,const MeshStatsMsg& s)  { return _tx(m,&s,sizeof(s)); }
    bool broadcastHeartbeat(const MeshHeartbeatMsg& h)       { return _tx(ESPNOW_BROADCAST,&h,sizeof(h)); }
    bool broadcastElection(const MeshElectMsg& e)            { return _tx(ESPNOW_BROADCAST,&e,sizeof(e)); }

    bool addPeer(const uint8_t* mac){
        bool bc=true; for(int i=0;i<6;i++) if(mac[i]!=0xFF){bc=false;break;} if(bc) return true;
        for(int i=0;i<_peerCount;i++) if(memcmp(_peers[i].mac,mac,6)==0) return true;
        if(_peerCount>=MESH_MAX_PEERS) return false;
        if(!esp_now_is_peer_exist(mac)){
            esp_now_peer_info_t pi={}; memcpy(pi.peer_addr,mac,6);
            pi.channel=_channel; pi.encrypt=false;
            if(esp_now_add_peer(&pi)!=ESP_OK) return false;
        }
        PeerInfo& p=_peers[_peerCount++];
        memcpy(p.mac,mac,6); p.active=true; p.last_seen_ms=millis(); p.hash_rate=0; p.assigned_chunk=0;
        Serial.printf("[Mesh] +peer %02X:%02X:%02X:%02X:%02X:%02X\n",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        _newPeer=true;   // signal main loop to redispatch
        return true;
    }

    void removeStalePeers(uint32_t now){
        for(int i=0;i<_peerCount;i++)
            if(_peers[i].active&&(now-_peers[i].last_seen_ms)>PEER_TIMEOUT_MS)
                _peers[i].active=false;
    }

    int  peerCount() const { int c=0; for(int i=0;i<_peerCount;i++) if(_peers[i].active) c++; return c; }

    bool getPeer(int idx,PeerInfo& out) const {
        int a=0;
        for(int i=0;i<_peerCount;i++) if(_peers[i].active){ if(a==idx){out=_peers[i];return true;} a++; }
        return false;
    }

    void updatePeerStats(const uint8_t* mac,uint32_t hr,uint32_t now){
        for(int i=0;i<_peerCount;i++) if(memcmp(_peers[i].mac,mac,6)==0){
            _peers[i].hash_rate=hr; _peers[i].last_seen_ms=now; _peers[i].active=true; return;
        }
        addPeer(mac); updatePeerStats(mac,hr,now);
    }

    void setMasterMac(const uint8_t* mac){ memcpy(_masterMac,mac,6); _hasMaster=true; addPeer(mac); }
    bool getMasterMac(uint8_t* out) const { if(!_hasMaster) return false; memcpy(out,_masterMac,6); return true; }
    bool hasMaster() const { return _hasMaster; }

    std::function<void(const MeshJobMsg&)>        onJob;
    std::function<void(const MeshResultMsg&)>     onResult;
    std::function<void(const MeshStatsMsg&)>      onStats;
    std::function<void(const MeshHeartbeatMsg&)>  onHeartbeat;
    std::function<void(const MeshElectMsg&)>      onElect;

    // Check and clear the new-peer flag (set by ISR when a worker is first seen)
    bool takeNewPeer(){ if(_newPeer){_newPeer=false;return true;} return false; }

private:
    EspNowMesh()=default;

    bool _tx(const uint8_t* mac,const void* data,size_t len){
        if(!_initialized||len>MESH_FRAME_LEN) return false;
        // Re-sync all peer channel registrations if WiFi channel has drifted.
        // The AP can change channels at any time; stale peer channels cause every send to fail.
        uint8_t curCh; wifi_second_chan_t sec;
        if(esp_wifi_get_channel(&curCh,&sec)==ESP_OK && curCh!=0 && curCh!=_channel){
            Serial.printf("[Mesh] channel drift %d->%d, resyncing peers\n",(int)_channel,(int)curCh);
            _channel=curCh;
            esp_wifi_set_channel(_channel,WIFI_SECOND_CHAN_NONE);
            esp_now_peer_info_t pi={}; pi.channel=_channel; pi.encrypt=false;
            // Update broadcast peer
            memcpy(pi.peer_addr,ESPNOW_BROADCAST,6);
            esp_now_mod_peer(&pi);
            // Update all unicast peers
            for(int i=0;i<_peerCount;i++){
                memcpy(pi.peer_addr,_peers[i].mac,6);
                esp_now_mod_peer(&pi);
            }
        }
        uint8_t frame[MESH_FRAME_LEN]={0}; memcpy(frame,data,len);
        return esp_now_send(mac,frame,MESH_FRAME_LEN)==ESP_OK;
    }

    // ESP32 core 3.x callback signatures
    static void _recv_cb(const esp_now_recv_info_t* info,const uint8_t* data,int len){
        if(len<1||!info) return;
        const uint8_t* mac=info->src_addr;
        EspNowMesh& m=instance(); m.addPeer(mac);
        uint8_t type=data[0];
        switch(type){
            case MSG_JOB:       if(len>=(int)sizeof(MeshJobMsg))      {MeshJobMsg       x;memcpy(&x,data,sizeof(x));if(m.onJob)      m.onJob(x);}      break;
            case MSG_RESULT:    if(len>=(int)sizeof(MeshResultMsg))   {MeshResultMsg    x;memcpy(&x,data,sizeof(x));if(m.onResult)   m.onResult(x);}   break;
            case MSG_STATS:     if(len>=(int)sizeof(MeshStatsMsg))    {MeshStatsMsg     x;memcpy(&x,data,sizeof(x));m.updatePeerStats(mac,x.hash_rate,millis());if(m.onStats)m.onStats(x);} break;
            case MSG_HEARTBEAT: if(len>=(int)sizeof(MeshHeartbeatMsg)){MeshHeartbeatMsg x;memcpy(&x,data,sizeof(x));
                                    if(x.is_master&&!m._hasMaster){m.setMasterMac(mac);Serial.printf("[Mesh] master %02X:%02X:%02X\n",mac[0],mac[1],mac[2]);}
                                    m.updatePeerStats(mac,x.total_hash_rate,millis());if(m.onHeartbeat)m.onHeartbeat(x);} break;
            case MSG_ELECT:     if(len>=(int)sizeof(MeshElectMsg))    {MeshElectMsg     x;memcpy(&x,data,sizeof(x));if(m.onElect)    m.onElect(x);}    break;
            default: break;
        }
    }
    static void _send_cb(const wifi_tx_info_t*,esp_now_send_status_t){}

    PeerInfo _peers[MESH_MAX_PEERS];
    int      _peerCount   = 0;
    bool     _initialized = false;
    bool     _isMaster    = false;
    bool     _hasMaster   = false;
    uint8_t  _masterMac[6]= {0};
    uint8_t  _channel     = ESPNOW_CHANNEL;
    volatile bool _newPeer = false;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 9 — STRATUM CLIENT
// ───────────────────────────────────────────────────────────────

class StratumClient {
public:
    bool connect(const char* host,uint16_t port,const char* user,const char* pass){
        Serial.printf("[Stratum] -> %s:%d\n",host,port);
        if(!_client.connect(host,port)){Serial.println("[Stratum] connection failed");return false;}
        _lastAct=millis();
        _tx("{\"id\":"+String(_id++)+",\"method\":\"mining.subscribe\",\"params\":[\"MeshMiner32/1.0\",null]}\n");
        uint32_t t=millis();
        while(millis()-t<STRATUM_TIMEOUT_MS){
            String line; if(_rxLine(line,500)){
                JsonDocument d;
                if(deserializeJson(d,line)==DeserializationError::Ok) _parseSub(d);
                if(_s.subscribed) break;
            }
            delay(10);
        }
        if(!_s.subscribed){Serial.println("[Stratum] subscribe timeout");return false;}
        _tx("{\"id\":"+String(_id++)+",\"method\":\"mining.authorize\",\"params\":[\""+String(user)+"\",\""+String(pass)+"\"]}\n");
        Serial.println("[Stratum] authorized, waiting for job...");
        return true;
    }

    void disconnect(){ _client.stop(); }
    bool isConnected(){ return _client.connected(); }

    void loop(){
        if(!_client.connected()) return;
        if(millis()-_lastAct>30000){
            _tx("{\"id\":"+String(_id++)+",\"method\":\"mining.ping\",\"params\":[]}\n");
            _lastAct=millis();
        }
        while(_client.available()){
            char c=_client.read();
            if(c=='\n'){if(_buf.length()>0)_procLine(_buf);_buf="";}
            else _buf+=c;
            _lastAct=millis();
        }
    }

    bool submit(const StratumJob& job,uint32_t nonce,const uint8_t* en2,uint8_t en2len){
        String msg="{\"id\":"+String(_id++)+",\"method\":\"mining.submit\",\"params\":[\""
            +String(POOL_USER)+"\",\""+String(job.job_id)+"\",\""
            +bytesToHex(en2,en2len)+"\",\""+bytesToHex(job.ntime,4)+"\",\""
            +bytesToHex((const uint8_t*)&nonce,4)+"\"]}\n";
        _submits++; return _tx(msg);
    }

    void computeMerkleRoot(uint8_t* root,const uint8_t* coinbase,uint16_t cbLen){
        double_sha256(coinbase,cbLen,root);
        for(int i=0;i<_job.merkle_count;i++){
            uint8_t pair[64]; memcpy(pair,root,32); memcpy(pair+32,_job.merkle_branches[i],32);
            double_sha256(pair,64,root);
        }
    }

    std::function<void(const StratumJob&)> onJob;

    const StratumJob&     currentJob()     const { return _job; }
    const StratumSession& session()        const { return _s; }
    uint32_t              acceptedShares() const { return _accepted; }
    uint32_t              totalSubmits()   const { return _submits; }

private:
    bool _tx(const String& msg){ if(!_client.connected()) return false; _client.print(msg); return true; }

    bool _rxLine(String& out,uint32_t tms){
        uint32_t t=millis();
        while(millis()-t<tms){if(_client.available()){out=_client.readStringUntil('\n');return out.length()>0;}delay(5);}
        return false;
    }

    void _procLine(const String& line){
        JsonDocument doc;
        if(deserializeJson(doc,line)!=DeserializationError::Ok) return;
        const char* method=doc["method"];
        if(method){
            if(strcmp(method,"mining.notify")==0)              _parseNotify(doc);
            else if(strcmp(method,"mining.set_difficulty")==0) _parseDiff(doc);
        } else {
            if((doc["result"]|false)&&(doc["id"]|0)>0) _accepted++;
        }
    }

    void _parseNotify(const JsonDocument& doc){
        JsonArrayConst p=doc["params"];
        if(p.isNull()||p.size()<9) return;
        StratumJob j={};
        strncpy(j.job_id,p[0]|"",sizeof(j.job_id)-1);
        hexToBytes(p[1]|"",j.prev_hash,32);
        j.coinbase1_len=strlen(p[2]|"")/2; hexToBytes(p[2]|"",j.coinbase1,sizeof(j.coinbase1));
        j.coinbase2_len=strlen(p[3]|"")/2; hexToBytes(p[3]|"",j.coinbase2,sizeof(j.coinbase2));
        JsonArrayConst br=p[4];
        j.merkle_count=min((int)br.size(),16);
        for(int i=0;i<j.merkle_count;i++) hexToBytes(br[i]|"",j.merkle_branches[i],32);
        hexToBytes(p[5]|"",j.version,4);
        hexToBytes(p[6]|"",j.nbits,4);
        hexToBytes(p[7]|"",j.ntime,4);
        j.clean_jobs=p[8]|false; j.valid=true;
        _job=j;
        Serial.printf("[Stratum] job %s  clean=%d\n",j.job_id,(int)j.clean_jobs);
        if(onJob) onJob(_job);
    }

    void _parseDiff(const JsonDocument& doc){
        JsonArrayConst p=doc["params"]; if(p.isNull()) return;
        _s.difficulty=p[0]|1;
        Serial.printf("[Stratum] difficulty %u\n",_s.difficulty);
    }

    void _parseSub(const JsonDocument& doc){
        JsonVariantConst r=doc["result"]; if(r.isNull()) return;
        const char* en1=r[1]|"";
        _s.extranonce1_len=min((int)(strlen(en1)/2),8);
        hexToBytes(en1,_s.extranonce1,_s.extranonce1_len);
        _s.extranonce2_len=r[2]|4;
        _s.subscribed=true;
        Serial.printf("[Stratum] subscribed  en1=%d  en2_size=%d\n",_s.extranonce1_len,_s.extranonce2_len);
    }

    WiFiClient     _client;
    StratumJob     _job    = {};
    StratumSession _s      = {};
    int            _id     = 1;
    uint32_t       _submits  = 0;
    uint32_t       _accepted = 0;
    uint32_t       _lastAct  = 0;
    String         _buf;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 10 — MINER  (FreeRTOS task, Core 1)
// ───────────────────────────────────────────────────────────────

struct MinerJob {
    uint8_t  header[80];
    uint8_t  target[32];
    uint32_t nonce_start;
    uint32_t nonce_end;
    uint8_t  job_id;
    uint8_t  stratum_job_id[64];
    uint8_t  extranonce2[8];
    uint8_t  extranonce2_len;
    bool     valid;
};

struct MinerStats {
    uint32_t hash_rate;
    uint32_t total_hashes;
    uint32_t found_nonces;
    uint32_t last_update_ms;
};

// Per-core context for dual-core mining
struct CoreCtx {
    volatile bool     newJob  = false;
    MinerJob          pending = {};
    uint32_t          hashRate= 0;
    uint32_t          totalH  = 0;
    uint32_t          found   = 0;
    SemaphoreHandle_t mutex   = nullptr;
};

class Miner {
public:
    static Miner& instance(){ static Miner m; return m; }

    void begin(){
        for(int c=0;c<2;c++) _ctx[c].mutex=xSemaphoreCreateMutex();
        // Core 1: away from WiFi stack (Core 0)
        xTaskCreatePinnedToCore(_task,"mineC1",MINING_TASK_STACK,&_ctx[1],
            MINING_TASK_PRIORITY,nullptr,1);
        // Core 0: WiFi events preempt at priority 23, mining fills idle time
        xTaskCreatePinnedToCore(_task,"mineC0",MINING_TASK_STACK,&_ctx[0],
            MINING_TASK_PRIORITY,nullptr,0);
        Serial.println("[Miner] dual-core tasks started");
    }

    void setJob(const MinerJob& j){
        uint32_t half = j.nonce_start + (j.nonce_end - j.nonce_start) / 2;
        MinerJob j0=j; j0.nonce_end   = half;
        MinerJob j1=j; j1.nonce_start = half;
        for(int c=0;c<2;c++){
            MinerJob& jc=(c==0)?j0:j1;
            if(!_ctx[c].mutex) continue;
            xSemaphoreTake(_ctx[c].mutex,portMAX_DELAY);
            _ctx[c].pending=jc; _ctx[c].newJob=true;
            xSemaphoreGive(_ctx[c].mutex);
        }
    }

    MinerStats getStats() const {
        MinerStats s={};
        s.hash_rate      = _ctx[0].hashRate + _ctx[1].hashRate;
        s.total_hashes   = _ctx[0].totalH   + _ctx[1].totalH;
        s.found_nonces   = _ctx[0].found    + _ctx[1].found;
        s.last_update_ms = millis();
        return s;
    }

    std::function<void(const MinerJob&,uint32_t)> onFound;

private:
    Miner()=default;
    CoreCtx _ctx[2];

    static void _task(void* p){ _run(static_cast<CoreCtx*>(p)); vTaskDelete(nullptr); }

    static void _run(CoreCtx* ctx){
        uint8_t  hash[32];
        uint32_t cnt=0, wstart=millis();
        MinerJob job={};
        while(true){
            if(ctx->newJob&&ctx->mutex&&xSemaphoreTake(ctx->mutex,0)==pdTRUE){
                job=ctx->pending; ctx->newJob=false; cnt=0; wstart=millis();
                xSemaphoreGive(ctx->mutex);
                Serial.printf("[Miner%d] job=%d  %08lX-%08lX\n",
                    xPortGetCoreID(),(int)job.job_id,
                    (unsigned long)job.nonce_start,(unsigned long)job.nonce_end);
            }
            if(!job.valid){ vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            for(uint32_t n=job.nonce_start;n<job.nonce_end&&!ctx->newJob;n++){
                job.header[76]= n     &0xFF;
                job.header[77]=(n>> 8)&0xFF;
                job.header[78]=(n>>16)&0xFF;
                job.header[79]=(n>>24)&0xFF;
                bitcoin_hash(job.header,hash);
                cnt++;
                if(check_difficulty(hash,job.target)){
                    ctx->found++;
                    Serial.printf("[Miner%d] *** FOUND nonce=%08X ***\n",xPortGetCoreID(),n);
                    // Flag for Core 0 loop() to handle — safe cross-core handoff
                    Miner::instance()._notifyFound(job,n);
                }
                if((cnt&0x3FF)==0){
                    uint32_t el=millis()-wstart;
                    if(el>0) ctx->hashRate=(cnt*1000UL)/el;
                    ctx->totalH+=1024;
                    vTaskDelay(1);
                }
            }
            if(!ctx->newJob) job.valid=false;
            vTaskDelay(1);
        }
    }

    void _notifyFound(const MinerJob& job, uint32_t nonce){
        if(onFound) onFound(job,nonce);
    }
};

// ───────────────────────────────────────────────────────────────
//  SECTION 11 — SH1106 SPI OLED DISPLAY  (U8g2 HW SPI)
// ───────────────────────────────────────────────────────────────

enum DisplayPage { PAGE_MINING=0,PAGE_POOL,PAGE_NETWORK,PAGE_MESH,PAGE_COUNT };

struct DisplayData {
    uint32_t hash_rate,total_hashes,found_blocks,accepted_shares;
    char     job_id[16];
    char     pool_host[32]; uint16_t pool_port; uint32_t difficulty;
    char     ssid[32]; int8_t rssi; char ip[16];
    int      peer_count;
    uint32_t worker_hash_rates[8];
    uint8_t  worker_macs[8][6];
    bool     is_master;
    uint32_t uptime_s;
    bool     mining_active;  // true once miner has processed at least one batch
    uint32_t tick;           // increments each display refresh — drives animations
};

class OledDisplay {
public:
    static OledDisplay& instance(){ static OledDisplay o; return o; }

    void begin(){
        _u8g2.begin();
        _u8g2.setContrast(200);
        _on=true;
        _splash();
    }

    void showStatus(const char* l1,const char* l2=nullptr){
        if(!_on) return;
        _u8g2.clearBuffer();
        _u8g2.setFont(u8g2_font_6x10_tf);
        _u8g2.drawStr(0,14,l1);
        if(l2) _u8g2.drawStr(0,28,l2);
        _u8g2.sendBuffer();
    }

    void nextPage(){ _page=(DisplayPage)(((int)_page+1)%PAGE_COUNT); }
    void setPage(DisplayPage p){ _page=p; }

    void sleep(){
        if(_sleeping) return;
        _sleeping=true;
        _u8g2.setPowerSave(1);
        Serial.println("[Display] sleep");
    }

    void wake(){
        if(!_sleeping) return;
        _sleeping=false;
        _u8g2.setPowerSave(0);
        Serial.println("[Display] wake");
    }

    bool isSleeping() const { return _sleeping; }

    void update(const DisplayData& d){
        if(!_on||_sleeping) return;
        _u8g2.clearBuffer();
        switch(_page){
            case PAGE_MINING:  _drawMining(d);  break;
            case PAGE_POOL:    _drawPool(d);    break;
            case PAGE_NETWORK: _drawNet(d);     break;
            case PAGE_MESH:    _drawMesh(d);    break;
            default: break;
        }
        // page indicator dots — bottom right
        for(int i=0;i<PAGE_COUNT;i++){
            if(i==(int)_page) _u8g2.drawBox(122-(PAGE_COUNT-1-i)*6,61,4,3);
            else              _u8g2.drawFrame(122-(PAGE_COUNT-1-i)*6,61,4,3);
        }
        _u8g2.sendBuffer();
    }

private:
    OledDisplay()=default;

    // SPI OLED with no CS pin (CS tied to GND on module).
    // U8G2_SH1106 4W_HW_SPI: (rotation, cs, dc, reset)
    // Pass U8X8_PIN_NONE for cs — library skips the CS toggle.
    U8G2_SH1106_128X64_NONAME_F_4W_HW_SPI _u8g2{
        U8G2_R0,
        /* cs=   */ U8X8_PIN_NONE,
        /* dc=   */ OLED_DC,
        /* reset=*/ OLED_RST
    };

    DisplayPage _page    = PAGE_MINING;
    bool        _on      = false;
    bool        _sleeping= false;

    void _splash(){
        _u8g2.clearBuffer();
        // ── Bitcoin ₿ symbol drawn with primitives ──
        // Outer circle
        _u8g2.drawCircle(32,26,18);
        // Vertical stem top
        _u8g2.drawVLine(32,7,4);
        // Vertical stem bottom
        _u8g2.drawVLine(32,41,4);
        // Left vertical bar of B
        _u8g2.drawVLine(27,14,24);
        _u8g2.drawVLine(28,14,24);
        // Top bump of B
        _u8g2.drawHLine(28,14,8);
        _u8g2.drawHLine(28,23,8);
        _u8g2.drawVLine(36,14,9);
        // Bottom bump of B (wider)
        _u8g2.drawHLine(28,24,9);
        _u8g2.drawHLine(28,38,9);
        _u8g2.drawVLine(37,24,14);
        // Slight tilt lines for $ style
        _u8g2.drawVLine(31,7,4);
        _u8g2.drawVLine(31,41,4);
        // ── Name — right half of screen (x=56 to 128 = 72px wide) ──
        // u8g2_font_9x15_tf: each char ~9px wide
        // "MeshMiner" = 9 chars = ~63px  fits in 72px
        // "32"        = 2 chars = ~18px
        _u8g2.setFont(u8g2_font_9x15_tf);
        _u8g2.drawStr(57,26,"MeshMiner");
        _u8g2.drawStr(57,44,"32");
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(57,57,"public-pool.io");
        _u8g2.sendBuffer();
    }

    static String _fmtHR(uint32_t h){
        if(h>=1000000) return String(h/1000000.0f,1)+"MH/s";
        if(h>=1000)    return String(h/1000.0f,1)+"kH/s";
        return String(h)+"H/s";
    }
    static String _fmtUp(uint32_t s){
        if(s<60)   return String(s)+"s";
        if(s<3600) return String(s/60)+"m";
        return String(s/3600)+"h";
    }

    void _drawMining(const DisplayData& d){
        // ── Header: name + spinner + uptime — all one line ──
        _u8g2.setFont(u8g2_font_5x7_tf);
        // Spinner indicator (| / - \) when mining, * when idle
        const char* spin[]={"| ","/ ","- ","\\ "};
        const char* ind = d.mining_active ? spin[d.tick%4] : "* ";
        char hdr[28];
        snprintf(hdr,sizeof(hdr),"%sMeshMiner32",ind);
        _u8g2.drawStr(0,7,hdr);
        char upbuf[12]; snprintf(upbuf,12,"UP:%s",_fmtUp(d.uptime_s).c_str());
        _u8g2.drawStr(90,7,upbuf);
        _u8g2.drawHLine(0,9,128);

        // ── Hashrate (big font) ─────────────────────
        _u8g2.setFont(u8g2_font_logisoso16_tr);
        String hr=d.mining_active?_fmtHR(d.hash_rate):"--";
        _u8g2.drawStr(0,30,hr.c_str());

        // ── Stats rows ──────────────────────────────
        _u8g2.setFont(u8g2_font_5x7_tf);
        char p1[18]; snprintf(p1,18,"Peers:%-2d",d.peer_count);
        _u8g2.drawStr(0,44,p1);
        char p2[18]; snprintf(p2,18,"Acc:%-5lu",(unsigned long)d.accepted_shares);
        _u8g2.drawStr(66,44,p2);
        char p3[18]; snprintf(p3,18,"Blk:%-3lu",(unsigned long)d.found_blocks);
        _u8g2.drawStr(0,55,p3);
        if(strlen(d.job_id)){
            char jbuf[12]; snprintf(jbuf,12,"J:%.6s",d.job_id);
            _u8g2.drawStr(66,55,jbuf);
        }
    }

    void _drawPool(const DisplayData& d){
        // Inverted header bar
        _u8g2.drawBox(0,0,128,10);
        _u8g2.setColorIndex(0);
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(2,8,"POOL");
        _u8g2.setColorIndex(1);
        _u8g2.setFont(u8g2_font_6x10_tf);
        char h[22]; snprintf(h,sizeof(h),"%.21s",d.pool_host);
        _u8g2.drawStr(0,22,h);
        _u8g2.drawStr(0,34,("Port: "+String(d.pool_port)).c_str());
        _u8g2.drawStr(0,46,("Diff: "+String(d.difficulty)).c_str());
        _u8g2.drawStr(0,58,("Acc: "+String(d.accepted_shares)).c_str());
    }

    void _drawNet(const DisplayData& d){
        _u8g2.drawBox(0,0,128,10);
        _u8g2.setColorIndex(0);
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(2,8,"NETWORK");
        _u8g2.setColorIndex(1);
        _u8g2.setFont(u8g2_font_6x10_tf);
        char s[22]; snprintf(s,sizeof(s),"%.21s",d.ssid); _u8g2.drawStr(0,22,s);
        _u8g2.drawStr(0,34,d.ip);
        _u8g2.drawStr(0,46,("RSSI: "+String(d.rssi)+"dBm").c_str());
        _u8g2.drawStr(0,58,("Mesh: "+String(d.peer_count)+" nodes").c_str());
    }

    void _drawMesh(const DisplayData& d){
        _u8g2.drawBox(0,0,128,10);
        _u8g2.setColorIndex(0);
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(2,8,"MESH WORKERS");
        _u8g2.setColorIndex(1);
        int cnt=min(d.peer_count,6);
        for(int i=0;i<cnt;i++){
            char line[28];
            snprintf(line,sizeof(line),"%02X:%02X:%02X %s",
                d.worker_macs[i][3],d.worker_macs[i][4],d.worker_macs[i][5],
                _fmtHR(d.worker_hash_rates[i]).c_str());
            _u8g2.drawStr(0,20+i*9,line);
        }
        if(d.peer_count==0){
            _u8g2.setFont(u8g2_font_6x10_tf);
            _u8g2.drawStr(10,38,"No workers");
            _u8g2.drawStr(10,52,"yet");
        }
    }
};

// ───────────────────────────────────────────────────────────────
//  SECTION 12 — GLOBAL STATE
// ───────────────────────────────────────────────────────────────

static EspNowMesh&   mesh    = EspNowMesh::instance();
static Miner&        miner   = Miner::instance();
static OledDisplay&  disp    = OledDisplay::instance();
static StratumClient stratum;

static bool     isMaster          = false;
static uint8_t  myMac[6]          = {0};
static uint8_t  currentJobId      = 0;
static uint32_t totalWorkerHR     = 0;
static uint32_t foundBlocks       = 0;
static uint32_t acceptedShares    = 0;   // persistent across pool reconnects
static uint32_t startupMs         = 0;
static uint32_t lastHeartbeatMs   = 0;
static uint32_t lastDisplayMs     = 0;
static uint32_t lastStatsMs       = 0;
static uint32_t masterLastSeenMs  = 0;
static bool     electionInProgress= false;
static uint32_t en2Counter        = 0;
// Cross-core nonce submission (miner runs Core 1, WiFi/ESP-NOW on Core 0)
static volatile bool     pendingNonceReady = false;
static volatile uint32_t pendingNonce      = 0;
static volatile uint8_t  pendingNonceJob   = 0;
static StratumJob lastStratumJob  = {};
static int      lastPeerCount     = 0;      // detect new worker joins
static uint32_t lastRebroadcastMs = 0;      // periodic job re-send to workers
static bool     _pendingRetry     = false;  // second dispatch ~800 ms after first
static uint32_t _retryMs          = 0;
static uint32_t lastPageFlipMs    = 0;      // auto page cycling
static uint32_t lastPoolRetryMs   = 0;      // non-blocking pool reconnect timer
static uint32_t lastLedMs         = 0;      // LED blink timer
static bool     ledState          = false;
static uint32_t lastBtnMs         = 0;      // button debounce
static bool     lastBtnState      = true;   // INPUT_PULLUP -> idle=HIGH
static uint32_t lastActivityMs    = 0;      // display sleep timeout

// ───────────────────────────────────────────────────────────────
//  SECTION 13 — JOB DISPATCH  (after all types are complete)
// ───────────────────────────────────────────────────────────────

static void buildCoinbase(uint8_t* out,uint16_t& outLen,
                           const StratumJob& sj,const StratumSession& sess,
                           uint32_t en2cnt)
{
    uint16_t pos=0;
    memcpy(out+pos,sj.coinbase1,sj.coinbase1_len); pos+=sj.coinbase1_len;
    memcpy(out+pos,sess.extranonce1,sess.extranonce1_len); pos+=sess.extranonce1_len;
    for(int i=0;i<sess.extranonce2_len&&i<4;i++) out[pos++]=(en2cnt>>(i*8))&0xFF;
    memcpy(out+pos,sj.coinbase2,sj.coinbase2_len); pos+=sj.coinbase2_len;
    outLen=pos;
}

static void dispatchJob(const StratumJob& sj){
    lastStratumJob=sj;
    en2Counter++;
    const StratumSession& sess=stratum.session();

    static uint8_t coinbase[300]; uint16_t cbLen=0;  // static avoids stack overflow
    buildCoinbase(coinbase,cbLen,sj,sess,en2Counter);
    uint8_t merkleRoot[32];
    stratum.computeMerkleRoot(merkleRoot,coinbase,cbLen);

    int workers=mesh.peerCount();
    int slots=workers+1;
    // Use 64-bit divide so result is correct for all slot counts (avoids overflow)
    uint32_t chunk=(uint32_t)(0x100000000ULL/(uint64_t)slots);
    if(chunk==0) chunk=0xFFFFFFFFUL;  // safety fallback

    // Build and broadcast job to each worker with its own nonce slice
    MeshJobMsg jm={};
    jm.msg_type=MSG_JOB; jm.job_id=++currentJobId;
    memcpy(jm.version,   sj.version,  4);
    memcpy(jm.prev_hash, sj.prev_hash,32);
    memcpy(jm.merkle_root,merkleRoot, 32);
    memcpy(jm.nbits,     sj.nbits,   4);
    memcpy(jm.ntime,     sj.ntime,   4);
    jm.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) jm.extranonce2[i]=(en2Counter>>(i*8))&0xFF;
    jm.pool_difficulty=sess.difficulty;

    for(int i=0;i<workers;i++){
        PeerInfo peer; if(!mesh.getPeer(i,peer)) continue;
        jm.nonce_start=chunk*(uint32_t)(i+1);
        jm.nonce_end  =jm.nonce_start+chunk-1;
        jm.assigned_chunk=(uint8_t)(i+1);
        mesh.broadcastJob(jm);
    }

    // Master hashes chunk 0
    MinerJob mj={};
    mj.valid=true; mj.job_id=currentJobId;
    strncpy((char*)mj.stratum_job_id,sj.job_id,63);
    mj.nonce_start=0; mj.nonce_end=chunk-1;
    mj.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) mj.extranonce2[i]=(en2Counter>>(i*8))&0xFF;
    difficulty_to_target(sess.difficulty,mj.target);
    memcpy(mj.header,    sj.version,  4);
    memcpy(mj.header+4,  sj.prev_hash,32);
    memcpy(mj.header+36, merkleRoot,  32);
    memcpy(mj.header+68, sj.ntime,   4);
    memcpy(mj.header+72, sj.nbits,   4);
    miner.setJob(mj);

    Serial.printf("[Dispatch] job=%d  workers=%d  chunk=%08X  diff=%u\n",currentJobId,workers,chunk,sess.difficulty);
}

// Re-dispatch the current job to workers with fresh nonce slices.
// Called when a new worker joins or on the rebroadcast timer.
static void redispatchToWorkers(){
    if(!lastStratumJob.valid) return;
    int workers=mesh.peerCount();
    if(workers==0) return;
    int slots=workers+1;
    uint32_t chunk=(uint32_t)(0x100000000ULL/(uint64_t)slots);
    if(chunk==0) chunk=0xFFFFFFFFUL;

    MeshJobMsg jm={};
    jm.msg_type=MSG_JOB; jm.job_id=currentJobId;
    memcpy(jm.version,    lastStratumJob.version,  4);
    memcpy(jm.prev_hash,  lastStratumJob.prev_hash,32);
    const StratumSession& sess=stratum.session();
    static uint8_t coinbase[300]; uint16_t cbLen=0;  // static avoids stack overflow
    // reuse same en2Counter so workers hash complementary ranges
    uint16_t pos=0;
    memcpy(coinbase+pos,lastStratumJob.coinbase1,lastStratumJob.coinbase1_len); pos+=lastStratumJob.coinbase1_len;
    memcpy(coinbase+pos,sess.extranonce1,sess.extranonce1_len); pos+=sess.extranonce1_len;
    for(int i=0;i<sess.extranonce2_len&&i<4;i++) coinbase[pos++]=(en2Counter>>(i*8))&0xFF;
    memcpy(coinbase+pos,lastStratumJob.coinbase2,lastStratumJob.coinbase2_len); pos+=lastStratumJob.coinbase2_len;
    cbLen=pos;
    uint8_t merkleRoot[32];
    stratum.computeMerkleRoot(merkleRoot,coinbase,cbLen);

    memcpy(jm.merkle_root,merkleRoot,32);
    memcpy(jm.nbits,lastStratumJob.nbits,4);
    memcpy(jm.ntime,lastStratumJob.ntime,4);
    jm.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) jm.extranonce2[i]=(en2Counter>>(i*8))&0xFF;

    for(int i=0;i<workers;i++){
        PeerInfo peer; if(!mesh.getPeer(i,peer)) continue;
        jm.nonce_start=chunk*(uint32_t)(i+1);
        jm.nonce_end  =jm.nonce_start+chunk-1;
        jm.assigned_chunk=(uint8_t)(i+1);
        mesh.broadcastJob(jm);
        Serial.printf("[Redispatch] worker %d  nonce %08X-%08X\n",i+1,jm.nonce_start,jm.nonce_end);
    }

    // Shrink master's slice too
    MinerJob mj={};
    mj.valid=true; mj.job_id=currentJobId;
    strncpy((char*)mj.stratum_job_id,lastStratumJob.job_id,63);
    mj.nonce_start=0; mj.nonce_end=chunk-1;
    mj.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) mj.extranonce2[i]=(en2Counter>>(i*8))&0xFF;
    nbits_to_target(lastStratumJob.nbits,mj.target);
    memcpy(mj.header,    lastStratumJob.version,  4);
    memcpy(mj.header+4,  lastStratumJob.prev_hash,32);
    memcpy(mj.header+36, merkleRoot,              32);
    memcpy(mj.header+68, lastStratumJob.ntime,    4);
    memcpy(mj.header+72, lastStratumJob.nbits,    4);
    miner.setJob(mj);
    Serial.printf("[Redispatch] job=%d  workers=%d  chunk=%08X\n",currentJobId,workers,chunk);
}

// ───────────────────────────────────────────────────────────────
//  SECTION 14 — FALLBACK ELECTION
// ───────────────────────────────────────────────────────────────

static void checkElection(){
    if(isMaster||electionInProgress) return;
    uint32_t now=millis();
    if(masterLastSeenMs==0){masterLastSeenMs=now;return;}
    if(now-masterLastSeenMs>(uint32_t)(ELECTION_TRIGGER_COUNT*HEARTBEAT_INTERVAL_MS)){
        electionInProgress=true;
        Serial.println("[Election] master silent — electing");
        delay(random(0,ELECTION_BACKOFF_MAX_MS));
        MeshElectMsg e={};
        e.msg_type=MSG_ELECT; memcpy(e.candidate_mac,myMac,6);
        uint8_t pri=0; for(int i=0;i<6;i++) pri^=myMac[i];
        e.priority=pri;
        mesh.broadcastElection(e);
        delay(ELECTION_BACKOFF_MAX_MS);
        Serial.println("[Election] promoting to MASTER");
        isMaster=true;
        WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
        uint32_t t=millis();
        while(WiFi.status()!=WL_CONNECTED&&millis()-t<WIFI_TIMEOUT_MS) delay(200);
        if(WiFi.status()==WL_CONNECTED){
            stratum.onJob=[](const StratumJob& j){dispatchJob(j);};
            stratum.connect(POOL_HOST,POOL_PORT,POOL_USER,POOL_PASS);
            disp.showStatus("PROMOTED","Now master!");
        }
    }
}

// ───────────────────────────────────────────────────────────────
//  SECTION 15 — SETUP & LOOP
// ───────────────────────────────────────────────────────────────

// Actual WiFi channel — read after connect, ESP-NOW must match
static uint8_t g_wifiChannel = ESPNOW_CHANNEL;

static bool tryWifi(){
#if NODE_ROLE == ROLE_MASTER
    return true;
#elif NODE_ROLE == ROLE_WORKER
    return false;
#else
    Serial.printf("[Main] WiFi -> %s\n",WIFI_SSID);
    WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
    uint32_t t=millis();
    while(WiFi.status()!=WL_CONNECTED&&millis()-t<WIFI_TIMEOUT_MS){delay(200);Serial.print(".");}
    Serial.println();
    if(WiFi.status()==WL_CONNECTED){
        // Read the real channel the AP assigned — ESP-NOW MUST use this same channel
        wifi_second_chan_t second;
        esp_wifi_get_channel(&g_wifiChannel,&second);
        Serial.printf("[Main] IP: %s  WiFi channel: %d\n",
            WiFi.localIP().toString().c_str(),(int)g_wifiChannel);
        return true;
    }
    return false;
#endif
}

void setup(){
    Serial.begin(SERIAL_BAUD);
    delay(100);
    Serial.println("\n=== MeshMiner 32 ===");
    startupMs=millis();
    WiFi.macAddress(myMac);
    Serial.printf("[Main] MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
        myMac[0],myMac[1],myMac[2],myMac[3],myMac[4],myMac[5]);

    disp.begin();
    delay(1500);   // show splash

    isMaster=tryWifi();

    if(isMaster){
        // ── MASTER ────────────────────────────
        Serial.println("[Main] role=MASTER");
        disp.showStatus("MASTER","Mesh init...");
        mesh.begin(true, g_wifiChannel);

        stratum.onJob=[](const StratumJob& j){
            dispatchJob(j);
            // If workers were already seen before the first job arrived, send them work now
            if(mesh.peerCount()>0){
                Serial.printf("[Main] job arrived with %d workers waiting — redispatching\n",mesh.peerCount());
                redispatchToWorkers();
                lastRebroadcastMs=millis();
            }
        };

        mesh.onResult=[](const MeshResultMsg& r){
            Serial.printf("[Master] worker result nonce=%08X job=%d\n",r.nonce,r.job_id);
            if(lastStratumJob.valid){
                uint8_t en2[8]={};
                for(int i=0;i<4;i++) en2[i]=(en2Counter>>(i*8))&0xFF;
                stratum.submit(lastStratumJob,r.nonce,en2,stratum.session().extranonce2_len);
            }
            foundBlocks++;
        };

        mesh.onStats=[](const MeshStatsMsg&){
            totalWorkerHR=0;
            int cnt=mesh.peerCount();
            for(int i=0;i<cnt;i++){PeerInfo p;if(mesh.getPeer(i,p))totalWorkerHR+=p.hash_rate;}
        };

        miner.onFound=[](const MinerJob& job,uint32_t nonce){
            // DO NOT call WiFi/stratum here — we are on Core 1
            // Signal Core 0 via volatile flag to submit safely
            pendingNonce    = nonce;
            pendingNonceJob = job.job_id;
            pendingNonceReady = true;
            foundBlocks++;
        };

        disp.showStatus("MASTER","Connecting pool...");
        if(!stratum.connect(POOL_HOST,POOL_PORT,POOL_USER,POOL_PASS)){
            disp.showStatus("Pool FAILED","check POOL_USER");
            Serial.println("[Main] Pool connect failed — will retry in loop");
        } else {
            disp.showStatus("MASTER","Pool connected!");
        }

    } else {
        // ── WORKER ────────────────────────────
        Serial.println("[Main] role=WORKER");
        disp.showStatus("WORKER","Listening...");
        mesh.begin(false);

        mesh.onJob=[](const MeshJobMsg& j){
            Serial.printf("[Worker] job=%d  nonce %08X-%08X\n",j.job_id,j.nonce_start,j.nonce_end);
            MinerJob mj={};
            mj.valid=true; mj.job_id=j.job_id;
            mj.nonce_start=j.nonce_start; mj.nonce_end=j.nonce_end;
            mj.extranonce2_len=j.extranonce2_len;
            memcpy(mj.extranonce2,j.extranonce2,j.extranonce2_len);
            nbits_to_target(j.nbits,mj.target);  // kept for block validation
            difficulty_to_target(j.pool_difficulty,mj.target);  // override with pool share diff
            memcpy(mj.header,    j.version,    4);
            memcpy(mj.header+4,  j.prev_hash,  32);
            memcpy(mj.header+36, j.merkle_root,32);
            memcpy(mj.header+68, j.ntime,      4);
            memcpy(mj.header+72, j.nbits,      4);
            miner.setJob(mj);
        };

        miner.onFound=[](const MinerJob& job,uint32_t nonce){
            // Signal Core 0 via flag — esp_now_send is not safe from Core 1
            pendingNonce    = nonce;
            pendingNonceJob = job.job_id;
            pendingNonceReady = true;
            foundBlocks++;
        };

        mesh.onHeartbeat=[](const MeshHeartbeatMsg& hb){
            if(hb.is_master){masterLastSeenMs=millis();electionInProgress=false;}
        };
    }

    miner.begin();

    // BOOT button (GPIO 0) for manual page flip — INPUT_PULLUP
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    // Blue LED — blinks at hash speed
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    lastPageFlipMs = millis();   // prevent instant flip on first loop tick
    lastActivityMs = millis();   // reset sleep timer at boot
    disp.setPage(PAGE_MINING);   // always start on the main mining page

    Serial.println("[Main] setup complete");
}

void loop(){
    uint32_t now=millis();

    // Cross-core nonce submission — handle found nonce from miner (Core 1) safely on Core 0
    if(pendingNonceReady){
        pendingNonceReady=false;
        uint32_t nonce = pendingNonce;
        Serial.printf("[Found] nonce=%08X job=%d\n", nonce, (int)pendingNonceJob);
        if(isMaster){
            if(lastStratumJob.valid){
                uint8_t en2[8]={};
                for(int i=0;i<4;i++) en2[i]=(en2Counter>>(i*8))&0xFF;
                stratum.submit(lastStratumJob,nonce,en2,stratum.session().extranonce2_len);
            }
        } else {
            uint8_t masterMac[6];
            if(mesh.getMasterMac(masterMac)){
                MeshResultMsg r={};
                r.msg_type=MSG_RESULT; r.job_id=pendingNonceJob; r.nonce=nonce;
                memcpy(r.worker_mac,myMac,6);
                mesh.sendResult(masterMac,r);
            }
        }
    }

    // Pool keep-alive + receive (master only)
    if(isMaster){
        if(!stratum.isConnected()){
            // Non-blocking retry — don't block the loop for 10s every tick
            if(now-lastPoolRetryMs>=POOL_RETRY_MS){
                lastPoolRetryMs=now;
                Serial.println("[Main] pool reconnecting...");
                stratum.connect(POOL_HOST,POOL_PORT,POOL_USER,POOL_PASS);
            }
        } else {
            stratum.loop();
        }
    }

    // Heartbeat broadcast
    if(now-lastHeartbeatMs>=HEARTBEAT_INTERVAL_MS){
        lastHeartbeatMs=now;
        MeshHeartbeatMsg hb={};
        hb.msg_type=MSG_HEARTBEAT;
        memcpy(hb.sender_mac,myMac,6);
        hb.is_master=isMaster?1:0;
        hb.uptime_s=(now-startupMs)/1000;
        hb.total_hash_rate=isMaster?totalWorkerHR+miner.getStats().hash_rate:miner.getStats().hash_rate;
        mesh.broadcastHeartbeat(hb);
        mesh.removeStalePeers(now);
    }

    // New worker joined — immediately re-slice and send jobs
    if(isMaster){
        // _newPeer flag is set inside the ESP-NOW ISR when addPeer() registers a new node
        if(mesh.takeNewPeer()){
            int curPeers=mesh.peerCount();   // read AFTER confirming new peer
            lastPeerCount=curPeers;
            Serial.printf("[Main] new peer -> %d workers\n",curPeers);
            if(lastStratumJob.valid){
                // Job already known — dispatch immediately
                Serial.println("[Main] job known, dispatching now");
                redispatchToWorkers();
                lastRebroadcastMs=now;
            } else {
                // Job not yet received — onJob callback will dispatch when it arrives
                Serial.println("[Main] waiting for first job from pool...");
            }
            // Always schedule a retry 1.5 s later as belt-and-braces
            _pendingRetry = true;
            _retryMs      = now + 1500;
        }
        // Retry dispatch ~800 ms after a new peer joined (belt-and-braces)
        if(_pendingRetry && now>=_retryMs){
            _pendingRetry=false;
            Serial.println("[Main] retry dispatch to new worker");
            redispatchToWorkers();
            lastRebroadcastMs=now;
        }
        // Periodic rebroadcast every 60s — catches workers that reset or missed a packet
        if(lastStratumJob.valid && mesh.peerCount()>0 && now-lastRebroadcastMs>=60000){
            lastRebroadcastMs=now;
            Serial.println("[Main] periodic rebroadcast");
            redispatchToWorkers();
        }
    }

    // Worker stats + election check
    if(!isMaster&&now-lastStatsMs>=5000){
        lastStatsMs=now;
        uint8_t masterMac[6];
        if(mesh.getMasterMac(masterMac)){
            MeshStatsMsg s={};
            s.msg_type=MSG_STATS;
            memcpy(s.worker_mac,myMac,6);
            s.hash_rate    =miner.getStats().hash_rate;
            s.nonces_tested=miner.getStats().total_hashes;
            s.job_id       =currentJobId;
            mesh.sendStats(masterMac,s);
        }
        checkElection();
    }

    // ── Display: page flip + refresh (unified block) ───────────
    {
        // Button check — BOOT button active LOW
        // 500ms debounce (GPIO0 can float on some DevKit variants)
        bool btn=digitalRead(BOOT_BTN_PIN);
        if(!btn && lastBtnState && (now-lastBtnMs)>500){
            lastBtnMs=now;
            lastActivityMs=now;          // reset sleep timer on any press
            if(disp.isSleeping()){
                disp.wake();             // first press just wakes — don't flip
                lastDisplayMs=0;         // force redraw immediately
                lastPageFlipMs=now;      // reset auto-flip timer
            } else {
                disp.nextPage();
                lastDisplayMs=0;
                lastPageFlipMs=now;
                Serial.println("[Display] btn flip");
            }
        }
        lastBtnState=btn;
    }

    // Display sleep timeout
    if(DISPLAY_SLEEP_MS > 0 && !disp.isSleeping() && (now-lastActivityMs)>=DISPLAY_SLEEP_MS){
        disp.sleep();
    }

    // Display refresh + auto page flip
    if(now-lastDisplayMs>=OLED_REFRESH_MS){
        lastDisplayMs=now;
        MinerStats ms=miner.getStats();
        DisplayData dd={};
        dd.hash_rate       =isMaster?totalWorkerHR+ms.hash_rate:ms.hash_rate;
        dd.total_hashes    =ms.total_hashes;
        dd.found_blocks    =foundBlocks;
        // Accumulate accepted shares — persists across reconnects
        {
            static uint32_t lastAcc=0;
            uint32_t curAcc=stratum.acceptedShares();
            if(curAcc>lastAcc){ acceptedShares+=curAcc-lastAcc; lastAcc=curAcc; }
        }
        dd.accepted_shares = acceptedShares;
        dd.is_master       =isMaster;
        dd.uptime_s        =(now-startupMs)/1000;
        dd.peer_count      =mesh.peerCount();
        strncpy(dd.pool_host,POOL_HOST,sizeof(dd.pool_host)-1);
        dd.pool_port       =POOL_PORT;
        dd.difficulty      =isMaster?stratum.session().difficulty:0;
        if(WiFi.status()==WL_CONNECTED){
            strncpy(dd.ssid,WiFi.SSID().c_str(),sizeof(dd.ssid)-1);
            strncpy(dd.ip,WiFi.localIP().toString().c_str(),sizeof(dd.ip)-1);
            dd.rssi=WiFi.RSSI();
        }
        int cnt=min(mesh.peerCount(),8);
        for(int i=0;i<cnt;i++){
            PeerInfo p;if(mesh.getPeer(i,p)){memcpy(dd.worker_macs[i],p.mac,6);dd.worker_hash_rates[i]=p.hash_rate;}
        }
        if(strlen(lastStratumJob.job_id)) strncpy(dd.job_id,lastStratumJob.job_id,15);
        dd.mining_active = (ms.total_hashes > 0);
        dd.tick          = (uint32_t)(now / 500);

        // Auto page flip — advance page every PAGE_FLIP_MS
        if(!disp.isSleeping() && now-lastPageFlipMs>=PAGE_FLIP_MS){
            lastPageFlipMs=now;
            disp.nextPage();
            Serial.printf("[Display] auto -> page %d\n", (int)((now/PAGE_FLIP_MS)%PAGE_COUNT));
        }

        disp.update(dd);
    }

    // Blue LED blinks at hash speed — faster = more hashes
    // At 30kH/s: blink every ~33ms. Cap at 50ms min so it's visible.
    {
        uint32_t hr = miner.getStats().hash_rate;
        uint32_t blinkInterval = (hr > 0) ? max(50UL, 1000000UL/hr) : 500;
        if(now-lastLedMs >= blinkInterval){
            lastLedMs=now;
            ledState=!ledState;
            digitalWrite(LED_PIN, ledState ? HIGH : LOW);
        }
    }

    delay(10);
}
*
 * ═══════════════════════════════════════════════════════════════
 *  MeshMiner32 Edition  —  Single-file Arduino sketch
 *  Target  : ESP32 DevKit V1  (esp32 board package 3.x)
 *  Pool    : public-pool.io:3333  (0% fee, verify at web.public-pool.io)
 *  Display : SH1106 128x64 — 4-wire Hardware SPI
 *
 *  Libraries (install via Arduino Library Manager):
 *    ArduinoJson  >= 7.0   (bblanchon)
 *    U8g2         >= 2.35  (olikraus)
 *
 *  SPI OLED wiring (ESP32 DevKit V1):
 *  ┌─────────────┬───────────────────────────────┐
 *  │  OLED pin   │  ESP32 pin                    │
 *  ├─────────────┼───────────────────────────────┤
 *  │  VCC        │  3.3V                         │
 *  │  GND        │  GND                          │
 *  │  CLK / D0   │  GPIO 18  (HW SPI SCK)        │
 *  │  MOSI / D1  │  GPIO 23  (HW SPI MOSI)       │
 *  │  CS         │  GND  (CS tied low on module) │
 *  │  DC         │  GPIO 22                      │
 *  │  RST        │  GPIO  4                      │
 *  └─────────────┴───────────────────────────────┘
 *
 *  Monitor your miner at: https://web.public-pool.io
 *  Enter your BTC address to see hashrate & shares live.
 * ═══════════════════════════════════════════════════════════════
 */

// ───────────────────────────────────────────────────────────────
//  SECTION 1 — USER CONFIGURATION  (edit these before flashing)
// ───────────────────────────────────────────────────────────────

#define WIFI_SSID        "Home Assistant"
#define WIFI_PASSWORD    "7537819ajk"
#define WIFI_TIMEOUT_MS  20000

// public-pool.io — 0% fee solo pool, tracks workers by BTC address
// Append a worker name after a dot so it shows in the dashboard
// e.g.  bc1qXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX.esp32master
#define POOL_HOST   "public-pool.io"
#define POOL_PORT   3333
#define POOL_USER   "bc1qdh02k3mrznn038g62arfpe8n42nk9uc96hcpty.MeshMiner32"
#define POOL_PASS   "x"

#define STRATUM_TIMEOUT_MS  10000

// Node role:
//   ROLE_AUTO   = tries WiFi → if connected becomes master, else worker
//   ROLE_MASTER = always master (needs WiFi creds above)
//   ROLE_WORKER = always worker  (no WiFi needed)
#define ROLE_AUTO    0
#define ROLE_MASTER  1
#define ROLE_WORKER  2
#define NODE_ROLE    ROLE_AUTO

// ── SH1106 SPI pins ──────────────────────────────────────────
//  SCL (CLK)  -> GPIO 18  (ESP32 HW SPI SCK,  label D18)
//  SDA (MOSI) -> GPIO 23  (ESP32 HW SPI MOSI, label D23)
//  DC         -> GPIO 22  (label D22)
//  RES        -> GPIO  4  (label D4)
//  CS         -> GND on module — use U8X8_PIN_NONE in code
#define OLED_DC   22
#define OLED_RST  4

#define OLED_REFRESH_MS   1000
#define PAGE_FLIP_MS      3000   // auto-advance page every N ms
#define DISPLAY_SLEEP_MS  30000  // turn off OLED after 30s of inactivity (0 = never)
#define BOOT_BTN_PIN        0    // GPIO0 = BOOT button on DevKit V1

// ── Mining task ──────────────────────────────────────────────
#define MINING_TASK_PRIORITY  5
#define MINING_TASK_CORE      1      // core 1 — WiFi stays on core 0
#define MINING_TASK_STACK     12288

// ── ESP-NOW mesh ─────────────────────────────────────────────
#define ESPNOW_CHANNEL          1
#define PEER_TIMEOUT_MS         15000
#define HEARTBEAT_INTERVAL_MS   5000
#define ELECTION_TRIGGER_COUNT  3
#define ELECTION_BACKOFF_MAX_MS 2000
#define MESH_MAX_PEERS          20
#define MESH_FRAME_LEN          250

#define SERIAL_BAUD       115200
#define LED_PIN           2    // Built-in blue LED on ESP32 DevKit V1
#define POOL_RETRY_MS  15000   // Only retry pool connection every 15s (non-blocking)

// ───────────────────────────────────────────────────────────────
//  SECTION 2 — INCLUDES
// ───────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <functional>
#include <string.h>

// ───────────────────────────────────────────────────────────────
//  SECTION 3 — FORWARD DECLARATIONS
//  (prevents Arduino IDE preprocessor ordering issues)
// ───────────────────────────────────────────────────────────────

struct StratumJob;
struct StratumSession;
struct MinerJob;
struct MinerStats;
struct PeerInfo;
struct DisplayData;

static void dispatchJob(const StratumJob& sj);
static void checkElection();

// ───────────────────────────────────────────────────────────────
//  SECTION 4 — SHA-256  (double SHA for Bitcoin mining)
// ───────────────────────────────────────────────────────────────

static const uint32_t SHA_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static const uint32_t SHA_H0[8] = {
    0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
};

#define ROTR32(x,n)    (((x)>>(n))|((x)<<(32-(n))))
#define SHA_CH(e,f,g)  (((e)&(f))^(~(e)&(g)))
#define SHA_MAJ(a,b,c) (((a)&(b))^((a)&(c))^((b)&(c)))
#define SHA_EP0(a)     (ROTR32(a,2) ^ROTR32(a,13)^ROTR32(a,22))
#define SHA_EP1(e)     (ROTR32(e,6) ^ROTR32(e,11)^ROTR32(e,25))
#define SHA_SIG0(x)    (ROTR32(x,7) ^ROTR32(x,18)^((x)>>3))
#define SHA_SIG1(x)    (ROTR32(x,17)^ROTR32(x,19)^((x)>>10))

static inline uint32_t sha_be32(const uint8_t* p){
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static inline void sha_wr32(uint8_t* p,uint32_t v){
    p[0]=(v>>24)&0xFF;p[1]=(v>>16)&0xFF;p[2]=(v>>8)&0xFF;p[3]=v&0xFF;
}

static void sha256_compress(const uint32_t* in16,uint32_t* st){
    uint32_t w[64];
    for(int i=0;i<16;i++) w[i]=in16[i];
    for(int i=16;i<64;i++) w[i]=SHA_SIG1(w[i-2])+w[i-7]+SHA_SIG0(w[i-15])+w[i-16];
    uint32_t a=st[0],b=st[1],c=st[2],d=st[3],e=st[4],f=st[5],g=st[6],h=st[7];
    for(int i=0;i<64;i++){
        uint32_t t1=h+SHA_EP1(e)+SHA_CH(e,f,g)+SHA_K[i]+w[i];
        uint32_t t2=SHA_EP0(a)+SHA_MAJ(a,b,c);
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    st[0]+=a;st[1]+=b;st[2]+=c;st[3]+=d;
    st[4]+=e;st[5]+=f;st[6]+=g;st[7]+=h;
}

static void double_sha256(const uint8_t* data,size_t len,uint8_t* digest){
    uint8_t mid[32];
    {
        uint32_t st[8]; memcpy(st,SHA_H0,32);
        uint8_t buf[64];
        size_t rem=len; const uint8_t* ptr=data;
        while(rem>=64){
            uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(ptr+i*4);
            sha256_compress(w,st); ptr+=64; rem-=64;
        }
        memset(buf,0,64); memcpy(buf,ptr,rem); buf[rem]=0x80;
        if(rem>=56){
            uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(buf+i*4);
            sha256_compress(w,st); memset(buf,0,64);
        }
        uint64_t bits=(uint64_t)len*8;
        for(int i=0;i<8;i++) buf[56+i]=(bits>>((7-i)*8))&0xFF;
        uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(buf+i*4);
        sha256_compress(w,st);
        for(int i=0;i<8;i++) sha_wr32(mid+i*4,st[i]);
    }
    {
        uint32_t st[8]; memcpy(st,SHA_H0,32);
        uint8_t buf[64]={0}; memcpy(buf,mid,32); buf[32]=0x80;
        buf[62]=0x01; buf[63]=0x00;
        uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(buf+i*4);
        sha256_compress(w,st);
        for(int i=0;i<8;i++) sha_wr32(digest+i*4,st[i]);
    }
}

static void bitcoin_hash(const uint8_t* h80,uint8_t* o32){ double_sha256(h80,80,o32); }

static bool check_difficulty(const uint8_t* hash,const uint8_t* tgt){
    for(int i=31;i>=0;i--){
        if(hash[i]<tgt[i]) return true;
        if(hash[i]>tgt[i]) return false;
    }
    return true;
}

static void nbits_to_target(const uint8_t* nb,uint8_t* t32){
    memset(t32,0,32); uint8_t exp=nb[0]; if(exp==0||exp>32) return;
    int pos=(int)exp-1;
    if(pos>=0   &&pos<32) t32[pos]  =nb[1];
    if(pos-1>=0 &&pos-1<32) t32[pos-1]=nb[2];
    if(pos-2>=0 &&pos-2<32) t32[pos-2]=nb[3];
}

// Convert stratum pool difficulty to a share target (index 31 = MSB).
// diff1 target = 0x00000000FFFF0000...0000  (0xFF at byte 27, 0xFF at byte 26)
// target_D = diff1_target / D  (long division, byte by byte from MSB)
static void difficulty_to_target(uint32_t diff, uint8_t* t32){
    memset(t32,0,32);
    if(diff==0) diff=1;
    uint64_t rem=0;
    for(int i=31;i>=0;i--){
        uint64_t cur=rem*256;
        if(i==27) cur+=0xFF;
        else if(i==26) cur+=0xFF;
        t32[i]=(uint8_t)(cur/diff);
        rem=cur%diff;
    }
}

// ───────────────────────────────────────────────────────────────
//  SECTION 5 — HEX HELPERS
// ───────────────────────────────────────────────────────────────

static void hexToBytes(const char* hex,uint8_t* out,int maxLen){
    int len=(int)(strlen(hex)/2); if(len>maxLen) len=maxLen;
    for(int i=0;i<len;i++){
        auto nib=[](char c)->uint8_t{
            if(c>='0'&&c<='9') return c-'0';
            if(c>='a'&&c<='f') return c-'a'+10;
            if(c>='A'&&c<='F') return c-'A'+10;
            return 0;
        };
        out[i]=(nib(hex[i*2])<<4)|nib(hex[i*2+1]);
    }
}

static String bytesToHex(const uint8_t* b,int len){
    static const char* H="0123456789abcdef";
    String s; s.reserve(len*2);
    for(int i=0;i<len;i++){s+=H[(b[i]>>4)&0xF];s+=H[b[i]&0xF];}
    return s;
}

// ───────────────────────────────────────────────────────────────
//  SECTION 6 — STRATUM STRUCTS
// ───────────────────────────────────────────────────────────────

struct StratumJob {
    char     job_id[64];
    uint8_t  prev_hash[32];
    uint8_t  coinbase1[128]; uint16_t coinbase1_len;
    uint8_t  coinbase2[128]; uint16_t coinbase2_len;
    uint8_t  merkle_branches[16][32];
    uint8_t  merkle_count;
    uint8_t  version[4];
    uint8_t  nbits[4];
    uint8_t  ntime[4];
    bool     clean_jobs;
    bool     valid;
};

struct StratumSession {
    uint8_t  extranonce1[8];
    uint8_t  extranonce1_len;
    uint8_t  extranonce2_len;
    uint32_t difficulty;
    bool     subscribed;
    bool     authorized;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 7 — ESP-NOW MESH STRUCTS
// ───────────────────────────────────────────────────────────────

#define MSG_JOB        0x01
#define MSG_RESULT     0x02
#define MSG_STATS      0x03
#define MSG_HEARTBEAT  0x04
#define MSG_ELECT      0x05

static const uint8_t ESPNOW_BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

#pragma pack(push,1)
typedef struct {
    uint8_t  msg_type;
    uint8_t  job_id;
    uint8_t  version[4];
    uint8_t  prev_hash[32];
    uint8_t  merkle_root[32];
    uint8_t  nbits[4];
    uint8_t  ntime[4];
    uint32_t nonce_start;
    uint32_t nonce_end;
    uint8_t  extranonce2[8];
    uint8_t  extranonce2_len;
    uint8_t  assigned_chunk;
    uint32_t pool_difficulty;   // stratum share difficulty — used for target, NOT nbits
} MeshJobMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  job_id;
    uint32_t nonce;
    uint8_t  worker_mac[6];
} MeshResultMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  worker_mac[6];
    uint32_t hash_rate;
    uint32_t nonces_tested;
    uint8_t  job_id;
} MeshStatsMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  sender_mac[6];
    uint8_t  is_master;
    uint32_t uptime_s;
    uint32_t total_hash_rate;
} MeshHeartbeatMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  candidate_mac[6];
    uint8_t  priority;
} MeshElectMsg;
#pragma pack(pop)

struct PeerInfo {
    uint8_t  mac[6];
    bool     active;
    uint32_t last_seen_ms;
    uint32_t hash_rate;
    uint8_t  assigned_chunk;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 8 — EspNowMesh CLASS
// ───────────────────────────────────────────────────────────────

class EspNowMesh {
public:
    static EspNowMesh& instance(){ static EspNowMesh i; return i; }

    bool begin(bool isMaster, uint8_t channel=ESPNOW_CHANNEL){
        _isMaster=isMaster;
        _channel=channel;
        if(isMaster) WiFi.mode(WIFI_AP_STA);
        else       { WiFi.mode(WIFI_STA); WiFi.disconnect(); }
        // Force ESP-NOW onto the same channel WiFi is using
        esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
        Serial.printf("[Mesh] ESP-NOW channel: %d\n",(int)_channel);
        if(esp_now_init()!=ESP_OK){ Serial.println("[Mesh] init failed"); return false; }
        esp_now_register_recv_cb(_recv_cb);
        esp_now_register_send_cb(_send_cb);
        esp_now_peer_info_t bc={};
        memcpy(bc.peer_addr,ESPNOW_BROADCAST,6);
        bc.channel=_channel; bc.encrypt=false;
        if(esp_now_add_peer(&bc)!=ESP_OK){ Serial.println("[Mesh] bcast fail"); return false; }
        _initialized=true;
        Serial.printf("[Mesh] ready as %s\n",isMaster?"MASTER":"WORKER");
        return true;
    }

    bool broadcastJob(const MeshJobMsg& j)                  { return _tx(ESPNOW_BROADCAST,&j,sizeof(j)); }
    bool sendResult(const uint8_t* m,const MeshResultMsg& r){ return _tx(m,&r,sizeof(r)); }
    bool sendStats(const uint8_t* m,const MeshStatsMsg& s)  { return _tx(m,&s,sizeof(s)); }
    bool broadcastHeartbeat(const MeshHeartbeatMsg& h)       { return _tx(ESPNOW_BROADCAST,&h,sizeof(h)); }
    bool broadcastElection(const MeshElectMsg& e)            { return _tx(ESPNOW_BROADCAST,&e,sizeof(e)); }

    bool addPeer(const uint8_t* mac){
        bool bc=true; for(int i=0;i<6;i++) if(mac[i]!=0xFF){bc=false;break;} if(bc) return true;
        for(int i=0;i<_peerCount;i++) if(memcmp(_peers[i].mac,mac,6)==0) return true;
        if(_peerCount>=MESH_MAX_PEERS) return false;
        if(!esp_now_is_peer_exist(mac)){
            esp_now_peer_info_t pi={}; memcpy(pi.peer_addr,mac,6);
            pi.channel=_channel; pi.encrypt=false;
            if(esp_now_add_peer(&pi)!=ESP_OK) return false;
        }
        PeerInfo& p=_peers[_peerCount++];
        memcpy(p.mac,mac,6); p.active=true; p.last_seen_ms=millis(); p.hash_rate=0; p.assigned_chunk=0;
        Serial.printf("[Mesh] +peer %02X:%02X:%02X:%02X:%02X:%02X\n",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        _newPeer=true;   // signal main loop to redispatch
        return true;
    }

    void removeStalePeers(uint32_t now){
        for(int i=0;i<_peerCount;i++)
            if(_peers[i].active&&(now-_peers[i].last_seen_ms)>PEER_TIMEOUT_MS)
                _peers[i].active=false;
    }

    int  peerCount() const { int c=0; for(int i=0;i<_peerCount;i++) if(_peers[i].active) c++; return c; }

    bool getPeer(int idx,PeerInfo& out) const {
        int a=0;
        for(int i=0;i<_peerCount;i++) if(_peers[i].active){ if(a==idx){out=_peers[i];return true;} a++; }
        return false;
    }

    void updatePeerStats(const uint8_t* mac,uint32_t hr,uint32_t now){
        for(int i=0;i<_peerCount;i++) if(memcmp(_peers[i].mac,mac,6)==0){
            _peers[i].hash_rate=hr; _peers[i].last_seen_ms=now; _peers[i].active=true; return;
        }
        addPeer(mac); updatePeerStats(mac,hr,now);
    }

    void setMasterMac(const uint8_t* mac){ memcpy(_masterMac,mac,6); _hasMaster=true; addPeer(mac); }
    bool getMasterMac(uint8_t* out) const { if(!_hasMaster) return false; memcpy(out,_masterMac,6); return true; }
    bool hasMaster() const { return _hasMaster; }

    std::function<void(const MeshJobMsg&)>        onJob;
    std::function<void(const MeshResultMsg&)>     onResult;
    std::function<void(const MeshStatsMsg&)>      onStats;
    std::function<void(const MeshHeartbeatMsg&)>  onHeartbeat;
    std::function<void(const MeshElectMsg&)>      onElect;

    // Check and clear the new-peer flag (set by ISR when a worker is first seen)
    bool takeNewPeer(){ if(_newPeer){_newPeer=false;return true;} return false; }

private:
    EspNowMesh()=default;

    bool _tx(const uint8_t* mac,const void* data,size_t len){
        if(!_initialized||len>MESH_FRAME_LEN) return false;
        // Re-sync all peer channel registrations if WiFi channel has drifted.
        // The AP can change channels at any time; stale peer channels cause every send to fail.
        uint8_t curCh; wifi_second_chan_t sec;
        if(esp_wifi_get_channel(&curCh,&sec)==ESP_OK && curCh!=0 && curCh!=_channel){
            Serial.printf("[Mesh] channel drift %d->%d, resyncing peers\n",(int)_channel,(int)curCh);
            _channel=curCh;
            esp_wifi_set_channel(_channel,WIFI_SECOND_CHAN_NONE);
            esp_now_peer_info_t pi={}; pi.channel=_channel; pi.encrypt=false;
            // Update broadcast peer
            memcpy(pi.peer_addr,ESPNOW_BROADCAST,6);
            esp_now_mod_peer(&pi);
            // Update all unicast peers
            for(int i=0;i<_peerCount;i++){
                memcpy(pi.peer_addr,_peers[i].mac,6);
                esp_now_mod_peer(&pi);
            }
        }
        uint8_t frame[MESH_FRAME_LEN]={0}; memcpy(frame,data,len);
        return esp_now_send(mac,frame,MESH_FRAME_LEN)==ESP_OK;
    }

    // ESP32 core 3.x callback signatures
    static void _recv_cb(const esp_now_recv_info_t* info,const uint8_t* data,int len){
        if(len<1||!info) return;
        const uint8_t* mac=info->src_addr;
        EspNowMesh& m=instance(); m.addPeer(mac);
        uint8_t type=data[0];
        switch(type){
            case MSG_JOB:       if(len>=(int)sizeof(MeshJobMsg))      {MeshJobMsg       x;memcpy(&x,data,sizeof(x));if(m.onJob)      m.onJob(x);}      break;
            case MSG_RESULT:    if(len>=(int)sizeof(MeshResultMsg))   {MeshResultMsg    x;memcpy(&x,data,sizeof(x));if(m.onResult)   m.onResult(x);}   break;
            case MSG_STATS:     if(len>=(int)sizeof(MeshStatsMsg))    {MeshStatsMsg     x;memcpy(&x,data,sizeof(x));m.updatePeerStats(mac,x.hash_rate,millis());if(m.onStats)m.onStats(x);} break;
            case MSG_HEARTBEAT: if(len>=(int)sizeof(MeshHeartbeatMsg)){MeshHeartbeatMsg x;memcpy(&x,data,sizeof(x));
                                    if(x.is_master&&!m._hasMaster){m.setMasterMac(mac);Serial.printf("[Mesh] master %02X:%02X:%02X\n",mac[0],mac[1],mac[2]);}
                                    m.updatePeerStats(mac,x.total_hash_rate,millis());if(m.onHeartbeat)m.onHeartbeat(x);} break;
            case MSG_ELECT:     if(len>=(int)sizeof(MeshElectMsg))    {MeshElectMsg     x;memcpy(&x,data,sizeof(x));if(m.onElect)    m.onElect(x);}    break;
            default: break;
        }
    }
    static void _send_cb(const wifi_tx_info_t*,esp_now_send_status_t){}

    PeerInfo _peers[MESH_MAX_PEERS];
    int      _peerCount   = 0;
    bool     _initialized = false;
    bool     _isMaster    = false;
    bool     _hasMaster   = false;
    uint8_t  _masterMac[6]= {0};
    uint8_t  _channel     = ESPNOW_CHANNEL;
    volatile bool _newPeer = false;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 9 — STRATUM CLIENT
// ───────────────────────────────────────────────────────────────

class StratumClient {
public:
    bool connect(const char* host,uint16_t port,const char* user,const char* pass){
        Serial.printf("[Stratum] -> %s:%d\n",host,port);
        if(!_client.connect(host,port)){Serial.println("[Stratum] connection failed");return false;}
        _lastAct=millis();
        _tx("{\"id\":"+String(_id++)+",\"method\":\"mining.subscribe\",\"params\":[\"MeshMiner32/1.0\",null]}\n");
        uint32_t t=millis();
        while(millis()-t<STRATUM_TIMEOUT_MS){
            String line; if(_rxLine(line,500)){
                JsonDocument d;
                if(deserializeJson(d,line)==DeserializationError::Ok) _parseSub(d);
                if(_s.subscribed) break;
            }
            delay(10);
        }
        if(!_s.subscribed){Serial.println("[Stratum] subscribe timeout");return false;}
        _tx("{\"id\":"+String(_id++)+",\"method\":\"mining.authorize\",\"params\":[\""+String(user)+"\",\""+String(pass)+"\"]}\n");
        Serial.println("[Stratum] authorized, waiting for job...");
        return true;
    }

    void disconnect(){ _client.stop(); }
    bool isConnected(){ return _client.connected(); }

    void loop(){
        if(!_client.connected()) return;
        if(millis()-_lastAct>30000){
            _tx("{\"id\":"+String(_id++)+",\"method\":\"mining.ping\",\"params\":[]}\n");
            _lastAct=millis();
        }
        while(_client.available()){
            char c=_client.read();
            if(c=='\n'){if(_buf.length()>0)_procLine(_buf);_buf="";}
            else _buf+=c;
            _lastAct=millis();
        }
    }

    bool submit(const StratumJob& job,uint32_t nonce,const uint8_t* en2,uint8_t en2len){
        String msg="{\"id\":"+String(_id++)+",\"method\":\"mining.submit\",\"params\":[\""
            +String(POOL_USER)+"\",\""+String(job.job_id)+"\",\""
            +bytesToHex(en2,en2len)+"\",\""+bytesToHex(job.ntime,4)+"\",\""
            +bytesToHex((const uint8_t*)&nonce,4)+"\"]}\n";
        _submits++; return _tx(msg);
    }

    void computeMerkleRoot(uint8_t* root,const uint8_t* coinbase,uint16_t cbLen){
        double_sha256(coinbase,cbLen,root);
        for(int i=0;i<_job.merkle_count;i++){
            uint8_t pair[64]; memcpy(pair,root,32); memcpy(pair+32,_job.merkle_branches[i],32);
            double_sha256(pair,64,root);
        }
    }

    std::function<void(const StratumJob&)> onJob;

    const StratumJob&     currentJob()     const { return _job; }
    const StratumSession& session()        const { return _s; }
    uint32_t              acceptedShares() const { return _accepted; }
    uint32_t              totalSubmits()   const { return _submits; }

private:
    bool _tx(const String& msg){ if(!_client.connected()) return false; _client.print(msg); return true; }

    bool _rxLine(String& out,uint32_t tms){
        uint32_t t=millis();
        while(millis()-t<tms){if(_client.available()){out=_client.readStringUntil('\n');return out.length()>0;}delay(5);}
        return false;
    }

    void _procLine(const String& line){
        JsonDocument doc;
        if(deserializeJson(doc,line)!=DeserializationError::Ok) return;
        const char* method=doc["method"];
        if(method){
            if(strcmp(method,"mining.notify")==0)              _parseNotify(doc);
            else if(strcmp(method,"mining.set_difficulty")==0) _parseDiff(doc);
        } else {
            if((doc["result"]|false)&&(doc["id"]|0)>0) _accepted++;
        }
    }

    void _parseNotify(const JsonDocument& doc){
        JsonArrayConst p=doc["params"];
        if(p.isNull()||p.size()<9) return;
        StratumJob j={};
        strncpy(j.job_id,p[0]|"",sizeof(j.job_id)-1);
        hexToBytes(p[1]|"",j.prev_hash,32);
        j.coinbase1_len=strlen(p[2]|"")/2; hexToBytes(p[2]|"",j.coinbase1,sizeof(j.coinbase1));
        j.coinbase2_len=strlen(p[3]|"")/2; hexToBytes(p[3]|"",j.coinbase2,sizeof(j.coinbase2));
        JsonArrayConst br=p[4];
        j.merkle_count=min((int)br.size(),16);
        for(int i=0;i<j.merkle_count;i++) hexToBytes(br[i]|"",j.merkle_branches[i],32);
        hexToBytes(p[5]|"",j.version,4);
        hexToBytes(p[6]|"",j.nbits,4);
        hexToBytes(p[7]|"",j.ntime,4);
        j.clean_jobs=p[8]|false; j.valid=true;
        _job=j;
        Serial.printf("[Stratum] job %s  clean=%d\n",j.job_id,(int)j.clean_jobs);
        if(onJob) onJob(_job);
    }

    void _parseDiff(const JsonDocument& doc){
        JsonArrayConst p=doc["params"]; if(p.isNull()) return;
        _s.difficulty=p[0]|1;
        Serial.printf("[Stratum] difficulty %u\n",_s.difficulty);
    }

    void _parseSub(const JsonDocument& doc){
        JsonVariantConst r=doc["result"]; if(r.isNull()) return;
        const char* en1=r[1]|"";
        _s.extranonce1_len=min((int)(strlen(en1)/2),8);
        hexToBytes(en1,_s.extranonce1,_s.extranonce1_len);
        _s.extranonce2_len=r[2]|4;
        _s.subscribed=true;
        Serial.printf("[Stratum] subscribed  en1=%d  en2_size=%d\n",_s.extranonce1_len,_s.extranonce2_len);
    }

    WiFiClient     _client;
    StratumJob     _job    = {};
    StratumSession _s      = {};
    int            _id     = 1;
    uint32_t       _submits  = 0;
    uint32_t       _accepted = 0;
    uint32_t       _lastAct  = 0;
    String         _buf;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 10 — MINER  (FreeRTOS task, Core 1)
// ───────────────────────────────────────────────────────────────

struct MinerJob {
    uint8_t  header[80];
    uint8_t  target[32];
    uint32_t nonce_start;
    uint32_t nonce_end;
    uint8_t  job_id;
    uint8_t  stratum_job_id[64];
    uint8_t  extranonce2[8];
    uint8_t  extranonce2_len;
    bool     valid;
};

struct MinerStats {
    uint32_t hash_rate;
    uint32_t total_hashes;
    uint32_t found_nonces;
    uint32_t last_update_ms;
};

class Miner {
public:
    static Miner& instance(){ static Miner m; return m; }

    void begin(){
        _mutex=xSemaphoreCreateMutex();
        xTaskCreatePinnedToCore(_task,"mining",MINING_TASK_STACK,this,MINING_TASK_PRIORITY,nullptr,MINING_TASK_CORE);
        Serial.printf("[Miner] task on core %d\n",MINING_TASK_CORE);
    }

    void setJob(const MinerJob& j){
        if(!_mutex) return;
        xSemaphoreTake(_mutex,portMAX_DELAY); _pending=j; _newJob=true; xSemaphoreGive(_mutex);
    }

    MinerStats getStats() const { return _stats; }

    std::function<void(const MinerJob&,uint32_t)> onFound;

private:
    Miner()=default;
    static void _task(void* p){ static_cast<Miner*>(p)->_run(); vTaskDelete(nullptr); }

    void _run(){
        uint8_t hash[32];
        uint32_t cnt=0,wstart=millis();
        while(true){
            if(_newJob&&_mutex&&xSemaphoreTake(_mutex,0)==pdTRUE){
                _job=_pending; _newJob=false; cnt=0; wstart=millis(); xSemaphoreGive(_mutex);
                Serial.printf("[Miner] job=%d  %08X-%08X\n",_job.job_id,_job.nonce_start,_job.nonce_end);
            }
            if(!_job.valid){ vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            for(uint32_t n=_job.nonce_start;n<_job.nonce_end&&!_newJob;n++){
                _job.header[76]= n     &0xFF;
                _job.header[77]=(n>> 8)&0xFF;
                _job.header[78]=(n>>16)&0xFF;
                _job.header[79]=(n>>24)&0xFF;
                bitcoin_hash(_job.header,hash);
                cnt++;
                if(check_difficulty(hash,_job.target)){
                    _stats.found_nonces++;
                    Serial.printf("[Miner] *** FOUND nonce=%08X ***\n",n);
                    if(onFound) onFound(_job,n);
                }
                if((cnt&0x3FF)==0){
                    uint32_t el=millis()-wstart;
                    if(el>0) _stats.hash_rate=(cnt*1000UL)/el;
                    _stats.total_hashes+=1024;
                    _stats.last_update_ms=millis();
                    // vTaskDelay(1) feeds the watchdog AND yields properly
                    vTaskDelay(1);
                }
            }
            if(!_newJob) _job.valid=false;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    volatile bool     _newJob  = false;
    MinerJob          _job     = {};
    MinerJob          _pending = {};
    MinerStats        _stats   = {};
    SemaphoreHandle_t _mutex   = nullptr;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 11 — SH1106 SPI OLED DISPLAY  (U8g2 HW SPI)
// ───────────────────────────────────────────────────────────────

enum DisplayPage { PAGE_MINING=0,PAGE_POOL,PAGE_NETWORK,PAGE_MESH,PAGE_COUNT };

struct DisplayData {
    uint32_t hash_rate,total_hashes,found_blocks,accepted_shares;
    char     job_id[16];
    char     pool_host[32]; uint16_t pool_port; uint32_t difficulty;
    char     ssid[32]; int8_t rssi; char ip[16];
    int      peer_count;
    uint32_t worker_hash_rates[8];
    uint8_t  worker_macs[8][6];
    bool     is_master;
    uint32_t uptime_s;
    bool     mining_active;  // true once miner has processed at least one batch
    uint32_t tick;           // increments each display refresh — drives animations
};

class OledDisplay {
public:
    static OledDisplay& instance(){ static OledDisplay o; return o; }

    void begin(){
        _u8g2.begin();
        _u8g2.setContrast(200);
        _on=true;
        _splash();
    }

    void showStatus(const char* l1,const char* l2=nullptr){
        if(!_on) return;
        _u8g2.clearBuffer();
        _u8g2.setFont(u8g2_font_6x10_tf);
        _u8g2.drawStr(0,14,l1);
        if(l2) _u8g2.drawStr(0,28,l2);
        _u8g2.sendBuffer();
    }

    void nextPage(){ _page=(DisplayPage)(((int)_page+1)%PAGE_COUNT); }
    void setPage(DisplayPage p){ _page=p; }

    void sleep(){
        if(_sleeping) return;
        _sleeping=true;
        _u8g2.setPowerSave(1);
        Serial.println("[Display] sleep");
    }

    void wake(){
        if(!_sleeping) return;
        _sleeping=false;
        _u8g2.setPowerSave(0);
        Serial.println("[Display] wake");
    }

    bool isSleeping() const { return _sleeping; }

    void update(const DisplayData& d){
        if(!_on||_sleeping) return;
        _u8g2.clearBuffer();
        switch(_page){
            case PAGE_MINING:  _drawMining(d);  break;
            case PAGE_POOL:    _drawPool(d);    break;
            case PAGE_NETWORK: _drawNet(d);     break;
            case PAGE_MESH:    _drawMesh(d);    break;
            default: break;
        }
        // page indicator dots — bottom right
        for(int i=0;i<PAGE_COUNT;i++){
            if(i==(int)_page) _u8g2.drawBox(122-(PAGE_COUNT-1-i)*6,61,4,3);
            else              _u8g2.drawFrame(122-(PAGE_COUNT-1-i)*6,61,4,3);
        }
        _u8g2.sendBuffer();
    }

private:
    OledDisplay()=default;

    // SPI OLED with no CS pin (CS tied to GND on module).
    // U8G2_SH1106 4W_HW_SPI: (rotation, cs, dc, reset)
    // Pass U8X8_PIN_NONE for cs — library skips the CS toggle.
    U8G2_SH1106_128X64_NONAME_F_4W_HW_SPI _u8g2{
        U8G2_R0,
        /* cs=   */ U8X8_PIN_NONE,
        /* dc=   */ OLED_DC,
        /* reset=*/ OLED_RST
    };

    DisplayPage _page    = PAGE_MINING;
    bool        _on      = false;
    bool        _sleeping= false;

    void _splash(){
        _u8g2.clearBuffer();
        // ── Bitcoin ₿ symbol drawn with primitives ──
        // Outer circle
        _u8g2.drawCircle(32,26,18);
        // Vertical stem top
        _u8g2.drawVLine(32,7,4);
        // Vertical stem bottom
        _u8g2.drawVLine(32,41,4);
        // Left vertical bar of B
        _u8g2.drawVLine(27,14,24);
        _u8g2.drawVLine(28,14,24);
        // Top bump of B
        _u8g2.drawHLine(28,14,8);
        _u8g2.drawHLine(28,23,8);
        _u8g2.drawVLine(36,14,9);
        // Bottom bump of B (wider)
        _u8g2.drawHLine(28,24,9);
        _u8g2.drawHLine(28,38,9);
        _u8g2.drawVLine(37,24,14);
        // Slight tilt lines for $ style
        _u8g2.drawVLine(31,7,4);
        _u8g2.drawVLine(31,41,4);
        // ── Name — right half of screen (x=56 to 128 = 72px wide) ──
        // u8g2_font_9x15_tf: each char ~9px wide
        // "MeshMiner" = 9 chars = ~63px  fits in 72px
        // "32"        = 2 chars = ~18px
        _u8g2.setFont(u8g2_font_9x15_tf);
        _u8g2.drawStr(57,26,"MeshMiner");
        _u8g2.drawStr(57,44,"32");
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(57,57,"public-pool.io");
        _u8g2.sendBuffer();
    }

    static String _fmtHR(uint32_t h){
        if(h>=1000000) return String(h/1000000.0f,1)+"MH/s";
        if(h>=1000)    return String(h/1000.0f,1)+"kH/s";
        return String(h)+"H/s";
    }
    static String _fmtUp(uint32_t s){
        if(s<60)   return String(s)+"s";
        if(s<3600) return String(s/60)+"m";
        return String(s/3600)+"h";
    }

    void _drawMining(const DisplayData& d){
        // ── Header: name + spinner + uptime — all one line ──
        _u8g2.setFont(u8g2_font_5x7_tf);
        // Spinner indicator (| / - \) when mining, * when idle
        const char* spin[]={"| ","/ ","- ","\\ "};
        const char* ind = d.mining_active ? spin[d.tick%4] : "* ";
        char hdr[28];
        snprintf(hdr,sizeof(hdr),"%sMeshMiner32",ind);
        _u8g2.drawStr(0,7,hdr);
        char upbuf[12]; snprintf(upbuf,12,"UP:%s",_fmtUp(d.uptime_s).c_str());
        _u8g2.drawStr(90,7,upbuf);
        _u8g2.drawHLine(0,9,128);

        // ── Hashrate (big font) ─────────────────────
        _u8g2.setFont(u8g2_font_logisoso16_tr);
        String hr=d.mining_active?_fmtHR(d.hash_rate):"--";
        _u8g2.drawStr(0,30,hr.c_str());

        // ── Stats rows ──────────────────────────────
        _u8g2.setFont(u8g2_font_5x7_tf);
        char p1[18]; snprintf(p1,18,"Peers:%-2d",d.peer_count);
        _u8g2.drawStr(0,44,p1);
        char p2[18]; snprintf(p2,18,"Acc:%-5lu",(unsigned long)d.accepted_shares);
        _u8g2.drawStr(66,44,p2);
        char p3[18]; snprintf(p3,18,"Blk:%-3lu",(unsigned long)d.found_blocks);
        _u8g2.drawStr(0,55,p3);
        if(strlen(d.job_id)){
            char jbuf[12]; snprintf(jbuf,12,"J:%.6s",d.job_id);
            _u8g2.drawStr(66,55,jbuf);
        }
    }

    void _drawPool(const DisplayData& d){
        // Inverted header bar
        _u8g2.drawBox(0,0,128,10);
        _u8g2.setColorIndex(0);
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(2,8,"POOL");
        _u8g2.setColorIndex(1);
        _u8g2.setFont(u8g2_font_6x10_tf);
        char h[22]; snprintf(h,sizeof(h),"%.21s",d.pool_host);
        _u8g2.drawStr(0,22,h);
        _u8g2.drawStr(0,34,("Port: "+String(d.pool_port)).c_str());
        _u8g2.drawStr(0,46,("Diff: "+String(d.difficulty)).c_str());
        _u8g2.drawStr(0,58,("Acc: "+String(d.accepted_shares)).c_str());
    }

    void _drawNet(const DisplayData& d){
        _u8g2.drawBox(0,0,128,10);
        _u8g2.setColorIndex(0);
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(2,8,"NETWORK");
        _u8g2.setColorIndex(1);
        _u8g2.setFont(u8g2_font_6x10_tf);
        char s[22]; snprintf(s,sizeof(s),"%.21s",d.ssid); _u8g2.drawStr(0,22,s);
        _u8g2.drawStr(0,34,d.ip);
        _u8g2.drawStr(0,46,("RSSI: "+String(d.rssi)+"dBm").c_str());
        _u8g2.drawStr(0,58,("Mesh: "+String(d.peer_count)+" nodes").c_str());
    }

    void _drawMesh(const DisplayData& d){
        _u8g2.drawBox(0,0,128,10);
        _u8g2.setColorIndex(0);
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(2,8,"MESH WORKERS");
        _u8g2.setColorIndex(1);
        int cnt=min(d.peer_count,6);
        for(int i=0;i<cnt;i++){
            char line[28];
            snprintf(line,sizeof(line),"%02X:%02X:%02X %s",
                d.worker_macs[i][3],d.worker_macs[i][4],d.worker_macs[i][5],
                _fmtHR(d.worker_hash_rates[i]).c_str());
            _u8g2.drawStr(0,20+i*9,line);
        }
        if(d.peer_count==0){
            _u8g2.setFont(u8g2_font_6x10_tf);
            _u8g2.drawStr(10,38,"No workers");
            _u8g2.drawStr(10,52,"yet");
        }
    }
};

// ───────────────────────────────────────────────────────────────
//  SECTION 12 — GLOBAL STATE
// ───────────────────────────────────────────────────────────────

static EspNowMesh&   mesh    = EspNowMesh::instance();
static Miner&        miner   = Miner::instance();
static OledDisplay&  disp    = OledDisplay::instance();
static StratumClient stratum;

static bool     isMaster          = false;
static uint8_t  myMac[6]          = {0};
static uint8_t  currentJobId      = 0;
static uint32_t totalWorkerHR     = 0;
static uint32_t foundBlocks       = 0;
static uint32_t acceptedShares    = 0;   // persistent across pool reconnects
static uint32_t startupMs         = 0;
static uint32_t lastHeartbeatMs   = 0;
static uint32_t lastDisplayMs     = 0;
static uint32_t lastStatsMs       = 0;
static uint32_t masterLastSeenMs  = 0;
static bool     electionInProgress= false;
static uint32_t en2Counter        = 0;
// Cross-core nonce submission (miner runs Core 1, WiFi/ESP-NOW on Core 0)
static volatile bool     pendingNonceReady = false;
static volatile uint32_t pendingNonce      = 0;
static volatile uint8_t  pendingNonceJob   = 0;
static StratumJob lastStratumJob  = {};
static int      lastPeerCount     = 0;      // detect new worker joins
static uint32_t lastRebroadcastMs = 0;      // periodic job re-send to workers
static bool     _pendingRetry     = false;  // second dispatch ~800 ms after first
static uint32_t _retryMs          = 0;
static uint32_t lastPageFlipMs    = 0;      // auto page cycling
static uint32_t lastPoolRetryMs   = 0;      // non-blocking pool reconnect timer
static uint32_t lastLedMs         = 0;      // LED blink timer
static bool     ledState          = false;
static uint32_t lastBtnMs         = 0;      // button debounce
static bool     lastBtnState      = true;   // INPUT_PULLUP -> idle=HIGH
static uint32_t lastActivityMs    = 0;      // display sleep timeout

// ───────────────────────────────────────────────────────────────
//  SECTION 13 — JOB DISPATCH  (after all types are complete)
// ───────────────────────────────────────────────────────────────

static void buildCoinbase(uint8_t* out,uint16_t& outLen,
                           const StratumJob& sj,const StratumSession& sess,
                           uint32_t en2cnt)
{
    uint16_t pos=0;
    memcpy(out+pos,sj.coinbase1,sj.coinbase1_len); pos+=sj.coinbase1_len;
    memcpy(out+pos,sess.extranonce1,sess.extranonce1_len); pos+=sess.extranonce1_len;
    for(int i=0;i<sess.extranonce2_len&&i<4;i++) out[pos++]=(en2cnt>>(i*8))&0xFF;
    memcpy(out+pos,sj.coinbase2,sj.coinbase2_len); pos+=sj.coinbase2_len;
    outLen=pos;
}

static void dispatchJob(const StratumJob& sj){
    lastStratumJob=sj;
    en2Counter++;
    const StratumSession& sess=stratum.session();

    static uint8_t coinbase[300]; uint16_t cbLen=0;  // static avoids stack overflow
    buildCoinbase(coinbase,cbLen,sj,sess,en2Counter);
    uint8_t merkleRoot[32];
    stratum.computeMerkleRoot(merkleRoot,coinbase,cbLen);

    int workers=mesh.peerCount();
    int slots=workers+1;
    // Use 64-bit divide so result is correct for all slot counts (avoids overflow)
    uint32_t chunk=(uint32_t)(0x100000000ULL/(uint64_t)slots);
    if(chunk==0) chunk=0xFFFFFFFFUL;  // safety fallback

    // Build and broadcast job to each worker with its own nonce slice
    MeshJobMsg jm={};
    jm.msg_type=MSG_JOB; jm.job_id=++currentJobId;
    memcpy(jm.version,   sj.version,  4);
    memcpy(jm.prev_hash, sj.prev_hash,32);
    memcpy(jm.merkle_root,merkleRoot, 32);
    memcpy(jm.nbits,     sj.nbits,   4);
    memcpy(jm.ntime,     sj.ntime,   4);
    jm.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) jm.extranonce2[i]=(en2Counter>>(i*8))&0xFF;
    jm.pool_difficulty=sess.difficulty;

    for(int i=0;i<workers;i++){
        PeerInfo peer; if(!mesh.getPeer(i,peer)) continue;
        jm.nonce_start=chunk*(uint32_t)(i+1);
        jm.nonce_end  =jm.nonce_start+chunk-1;
        jm.assigned_chunk=(uint8_t)(i+1);
        mesh.broadcastJob(jm);
    }

    // Master hashes chunk 0
    MinerJob mj={};
    mj.valid=true; mj.job_id=currentJobId;
    strncpy((char*)mj.stratum_job_id,sj.job_id,63);
    mj.nonce_start=0; mj.nonce_end=chunk-1;
    mj.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) mj.extranonce2[i]=(en2Counter>>(i*8))&0xFF;
    difficulty_to_target(sess.difficulty,mj.target);
    memcpy(mj.header,    sj.version,  4);
    memcpy(mj.header+4,  sj.prev_hash,32);
    memcpy(mj.header+36, merkleRoot,  32);
    memcpy(mj.header+68, sj.ntime,   4);
    memcpy(mj.header+72, sj.nbits,   4);
    miner.setJob(mj);

    Serial.printf("[Dispatch] job=%d  workers=%d  chunk=%08X  diff=%u\n",currentJobId,workers,chunk,sess.difficulty);
}

// Re-dispatch the current job to workers with fresh nonce slices.
// Called when a new worker joins or on the rebroadcast timer.
static void redispatchToWorkers(){
    if(!lastStratumJob.valid) return;
    int workers=mesh.peerCount();
    if(workers==0) return;
    int slots=workers+1;
    uint32_t chunk=(uint32_t)(0x100000000ULL/(uint64_t)slots);
    if(chunk==0) chunk=0xFFFFFFFFUL;

    MeshJobMsg jm={};
    jm.msg_type=MSG_JOB; jm.job_id=currentJobId;
    memcpy(jm.version,    lastStratumJob.version,  4);
    memcpy(jm.prev_hash,  lastStratumJob.prev_hash,32);
    const StratumSession& sess=stratum.session();
    static uint8_t coinbase[300]; uint16_t cbLen=0;  // static avoids stack overflow
    // reuse same en2Counter so workers hash complementary ranges
    uint16_t pos=0;
    memcpy(coinbase+pos,lastStratumJob.coinbase1,lastStratumJob.coinbase1_len); pos+=lastStratumJob.coinbase1_len;
    memcpy(coinbase+pos,sess.extranonce1,sess.extranonce1_len); pos+=sess.extranonce1_len;
    for(int i=0;i<sess.extranonce2_len&&i<4;i++) coinbase[pos++]=(en2Counter>>(i*8))&0xFF;
    memcpy(coinbase+pos,lastStratumJob.coinbase2,lastStratumJob.coinbase2_len); pos+=lastStratumJob.coinbase2_len;
    cbLen=pos;
    uint8_t merkleRoot[32];
    stratum.computeMerkleRoot(merkleRoot,coinbase,cbLen);

    memcpy(jm.merkle_root,merkleRoot,32);
    memcpy(jm.nbits,lastStratumJob.nbits,4);
    memcpy(jm.ntime,lastStratumJob.ntime,4);
    jm.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) jm.extranonce2[i]=(en2Counter>>(i*8))&0xFF;

    for(int i=0;i<workers;i++){
        PeerInfo peer; if(!mesh.getPeer(i,peer)) continue;
        jm.nonce_start=chunk*(uint32_t)(i+1);
        jm.nonce_end  =jm.nonce_start+chunk-1;
        jm.assigned_chunk=(uint8_t)(i+1);
        mesh.broadcastJob(jm);
        Serial.printf("[Redispatch] worker %d  nonce %08X-%08X\n",i+1,jm.nonce_start,jm.nonce_end);
    }

    // Shrink master's slice too
    MinerJob mj={};
    mj.valid=true; mj.job_id=currentJobId;
    strncpy((char*)mj.stratum_job_id,lastStratumJob.job_id,63);
    mj.nonce_start=0; mj.nonce_end=chunk-1;
    mj.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) mj.extranonce2[i]=(en2Counter>>(i*8))&0xFF;
    nbits_to_target(lastStratumJob.nbits,mj.target);
    memcpy(mj.header,    lastStratumJob.version,  4);
    memcpy(mj.header+4,  lastStratumJob.prev_hash,32);
    memcpy(mj.header+36, merkleRoot,              32);
    memcpy(mj.header+68, lastStratumJob.ntime,    4);
    memcpy(mj.header+72, lastStratumJob.nbits,    4);
    miner.setJob(mj);
    Serial.printf("[Redispatch] job=%d  workers=%d  chunk=%08X\n",currentJobId,workers,chunk);
}

// ───────────────────────────────────────────────────────────────
//  SECTION 14 — FALLBACK ELECTION
// ───────────────────────────────────────────────────────────────

static void checkElection(){
    if(isMaster||electionInProgress) return;
    uint32_t now=millis();
    if(masterLastSeenMs==0){masterLastSeenMs=now;return;}
    if(now-masterLastSeenMs>(uint32_t)(ELECTION_TRIGGER_COUNT*HEARTBEAT_INTERVAL_MS)){
        electionInProgress=true;
        Serial.println("[Election] master silent — electing");
        delay(random(0,ELECTION_BACKOFF_MAX_MS));
        MeshElectMsg e={};
        e.msg_type=MSG_ELECT; memcpy(e.candidate_mac,myMac,6);
        uint8_t pri=0; for(int i=0;i<6;i++) pri^=myMac[i];
        e.priority=pri;
        mesh.broadcastElection(e);
        delay(ELECTION_BACKOFF_MAX_MS);
        Serial.println("[Election] promoting to MASTER");
        isMaster=true;
        WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
        uint32_t t=millis();
        while(WiFi.status()!=WL_CONNECTED&&millis()-t<WIFI_TIMEOUT_MS) delay(200);
        if(WiFi.status()==WL_CONNECTED){
            stratum.onJob=[](const StratumJob& j){dispatchJob(j);};
            stratum.connect(POOL_HOST,POOL_PORT,POOL_USER,POOL_PASS);
            disp.showStatus("PROMOTED","Now master!");
        }
    }
}

// ───────────────────────────────────────────────────────────────
//  SECTION 15 — SETUP & LOOP
// ───────────────────────────────────────────────────────────────

// Actual WiFi channel — read after connect, ESP-NOW must match
static uint8_t g_wifiChannel = ESPNOW_CHANNEL;

static bool tryWifi(){
#if NODE_ROLE == ROLE_MASTER
    return true;
#elif NODE_ROLE == ROLE_WORKER
    return false;
#else
    Serial.printf("[Main] WiFi -> %s\n",WIFI_SSID);
    WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
    uint32_t t=millis();
    while(WiFi.status()!=WL_CONNECTED&&millis()-t<WIFI_TIMEOUT_MS){delay(200);Serial.print(".");}
    Serial.println();
    if(WiFi.status()==WL_CONNECTED){
        // Read the real channel the AP assigned — ESP-NOW MUST use this same channel
        wifi_second_chan_t second;
        esp_wifi_get_channel(&g_wifiChannel,&second);
        Serial.printf("[Main] IP: %s  WiFi channel: %d\n",
            WiFi.localIP().toString().c_str(),(int)g_wifiChannel);
        return true;
    }
    return false;
#endif
}

void setup(){
    Serial.begin(SERIAL_BAUD);
    delay(100);
    Serial.println("\n=== MeshMiner 32 ===");
    startupMs=millis();
    WiFi.macAddress(myMac);
    Serial.printf("[Main] MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
        myMac[0],myMac[1],myMac[2],myMac[3],myMac[4],myMac[5]);

    disp.begin();
    delay(1500);   // show splash

    isMaster=tryWifi();

    if(isMaster){
        // ── MASTER ────────────────────────────
        Serial.println("[Main] role=MASTER");
        disp.showStatus("MASTER","Mesh init...");
        mesh.begin(true, g_wifiChannel);

        stratum.onJob=[](const StratumJob& j){
            dispatchJob(j);
            // If workers were already seen before the first job arrived, send them work now
            if(mesh.peerCount()>0){
                Serial.printf("[Main] job arrived with %d workers waiting — redispatching\n",mesh.peerCount());
                redispatchToWorkers();
                lastRebroadcastMs=millis();
            }
        };

        mesh.onResult=[](const MeshResultMsg& r){
            Serial.printf("[Master] worker result nonce=%08X job=%d\n",r.nonce,r.job_id);
            if(lastStratumJob.valid){
                uint8_t en2[8]={};
                for(int i=0;i<4;i++) en2[i]=(en2Counter>>(i*8))&0xFF;
                stratum.submit(lastStratumJob,r.nonce,en2,stratum.session().extranonce2_len);
            }
            foundBlocks++;
        };

        mesh.onStats=[](const MeshStatsMsg&){
            totalWorkerHR=0;
            int cnt=mesh.peerCount();
            for(int i=0;i<cnt;i++){PeerInfo p;if(mesh.getPeer(i,p))totalWorkerHR+=p.hash_rate;}
        };

        miner.onFound=[](const MinerJob& job,uint32_t nonce){
            // DO NOT call WiFi/stratum here — we are on Core 1
            // Signal Core 0 via volatile flag to submit safely
            pendingNonce    = nonce;
            pendingNonceJob = job.job_id;
            pendingNonceReady = true;
            foundBlocks++;
        };

        disp.showStatus("MASTER","Connecting pool...");
        if(!stratum.connect(POOL_HOST,POOL_PORT,POOL_USER,POOL_PASS)){
            disp.showStatus("Pool FAILED","check POOL_USER");
            Serial.println("[Main] Pool connect failed — will retry in loop");
        } else {
            disp.showStatus("MASTER","Pool connected!");
        }

    } else {
        // ── WORKER ────────────────────────────
        Serial.println("[Main] role=WORKER");
        disp.showStatus("WORKER","Listening...");
        mesh.begin(false);

        mesh.onJob=[](const MeshJobMsg& j){
            Serial.printf("[Worker] job=%d  nonce %08X-%08X\n",j.job_id,j.nonce_start,j.nonce_end);
            MinerJob mj={};
            mj.valid=true; mj.job_id=j.job_id;
            mj.nonce_start=j.nonce_start; mj.nonce_end=j.nonce_end;
            mj.extranonce2_len=j.extranonce2_len;
            memcpy(mj.extranonce2,j.extranonce2,j.extranonce2_len);
            nbits_to_target(j.nbits,mj.target);  // kept for block validation
            difficulty_to_target(j.pool_difficulty,mj.target);  // override with pool share diff
            memcpy(mj.header,    j.version,    4);
            memcpy(mj.header+4,  j.prev_hash,  32);
            memcpy(mj.header+36, j.merkle_root,32);
            memcpy(mj.header+68, j.ntime,      4);
            memcpy(mj.header+72, j.nbits,      4);
            miner.setJob(mj);
        };

        miner.onFound=[](const MinerJob& job,uint32_t nonce){
            // Signal Core 0 via flag — esp_now_send is not safe from Core 1
            pendingNonce    = nonce;
            pendingNonceJob = job.job_id;
            pendingNonceReady = true;
            foundBlocks++;
        };

        mesh.onHeartbeat=[](const MeshHeartbeatMsg& hb){
            if(hb.is_master){masterLastSeenMs=millis();electionInProgress=false;}
        };
    }

    miner.begin();

    // BOOT button (GPIO 0) for manual page flip — INPUT_PULLUP
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    // Blue LED — blinks at hash speed
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    lastPageFlipMs = millis();   // prevent instant flip on first loop tick
    lastActivityMs = millis();   // reset sleep timer at boot
    disp.setPage(PAGE_MINING);   // always start on the main mining page

    Serial.println("[Main] setup complete");
}

void loop(){
    uint32_t now=millis();

    // Cross-core nonce submission — handle found nonce from miner (Core 1) safely on Core 0
    if(pendingNonceReady){
        pendingNonceReady=false;
        uint32_t nonce = pendingNonce;
        Serial.printf("[Found] nonce=%08X job=%d\n", nonce, (int)pendingNonceJob);
        if(isMaster){
            if(lastStratumJob.valid){
                uint8_t en2[8]={};
                for(int i=0;i<4;i++) en2[i]=(en2Counter>>(i*8))&0xFF;
                stratum.submit(lastStratumJob,nonce,en2,stratum.session().extranonce2_len);
            }
        } else {
            uint8_t masterMac[6];
            if(mesh.getMasterMac(masterMac)){
                MeshResultMsg r={};
                r.msg_type=MSG_RESULT; r.job_id=pendingNonceJob; r.nonce=nonce;
                memcpy(r.worker_mac,myMac,6);
                mesh.sendResult(masterMac,r);
            }
        }
    }

    // Pool keep-alive + receive (master only)
    if(isMaster){
        if(!stratum.isConnected()){
            // Non-blocking retry — don't block the loop for 10s every tick
            if(now-lastPoolRetryMs>=POOL_RETRY_MS){
                lastPoolRetryMs=now;
                Serial.println("[Main] pool reconnecting...");
                stratum.connect(POOL_HOST,POOL_PORT,POOL_USER,POOL_PASS);
            }
        } else {
            stratum.loop();
        }
    }

    // Heartbeat broadcast
    if(now-lastHeartbeatMs>=HEARTBEAT_INTERVAL_MS){
        lastHeartbeatMs=now;
        MeshHeartbeatMsg hb={};
        hb.msg_type=MSG_HEARTBEAT;
        memcpy(hb.sender_mac,myMac,6);
        hb.is_master=isMaster?1:0;
        hb.uptime_s=(now-startupMs)/1000;
        hb.total_hash_rate=isMaster?totalWorkerHR+miner.getStats().hash_rate:miner.getStats().hash_rate;
        mesh.broadcastHeartbeat(hb);
        mesh.removeStalePeers(now);
    }

    // New worker joined — immediately re-slice and send jobs
    if(isMaster){
        // _newPeer flag is set inside the ESP-NOW ISR when addPeer() registers a new node
        if(mesh.takeNewPeer()){
            int curPeers=mesh.peerCount();   // read AFTER confirming new peer
            lastPeerCount=curPeers;
            Serial.printf("[Main] new peer -> %d workers\n",curPeers);
            if(lastStratumJob.valid){
                // Job already known — dispatch immediately
                Serial.println("[Main] job known, dispatching now");
                redispatchToWorkers();
                lastRebroadcastMs=now;
            } else {
                // Job not yet received — onJob callback will dispatch when it arrives
                Serial.println("[Main] waiting for first job from pool...");
            }
            // Always schedule a retry 1.5 s later as belt-and-braces
            _pendingRetry = true;
            _retryMs      = now + 1500;
        }
        // Retry dispatch ~800 ms after a new peer joined (belt-and-braces)
        if(_pendingRetry && now>=_retryMs){
            _pendingRetry=false;
            Serial.println("[Main] retry dispatch to new worker");
            redispatchToWorkers();
            lastRebroadcastMs=now;
        }
        // Periodic rebroadcast every 60s — catches workers that reset or missed a packet
        if(lastStratumJob.valid && mesh.peerCount()>0 && now-lastRebroadcastMs>=60000){
            lastRebroadcastMs=now;
            Serial.println("[Main] periodic rebroadcast");
            redispatchToWorkers();
        }
    }

    // Worker stats + election check
    if(!isMaster&&now-lastStatsMs>=5000){
        lastStatsMs=now;
        uint8_t masterMac[6];
        if(mesh.getMasterMac(masterMac)){
            MeshStatsMsg s={};
            s.msg_type=MSG_STATS;
            memcpy(s.worker_mac,myMac,6);
            s.hash_rate    =miner.getStats().hash_rate;
            s.nonces_tested=miner.getStats().total_hashes;
            s.job_id       =currentJobId;
            mesh.sendStats(masterMac,s);
        }
        checkElection();
    }

    // ── Display: page flip + refresh (unified block) ───────────
    {
        // Button check — BOOT button active LOW
        // 500ms debounce (GPIO0 can float on some DevKit variants)
        bool btn=digitalRead(BOOT_BTN_PIN);
        if(!btn && lastBtnState && (now-lastBtnMs)>500){
            lastBtnMs=now;
            lastActivityMs=now;          // reset sleep timer on any press
            if(disp.isSleeping()){
                disp.wake();             // first press just wakes — don't flip
                lastDisplayMs=0;         // force redraw immediately
                lastPageFlipMs=now;      // reset auto-flip timer
            } else {
                disp.nextPage();
                lastDisplayMs=0;
                lastPageFlipMs=now;
                Serial.println("[Display] btn flip");
            }
        }
        lastBtnState=btn;
    }

    // Display sleep timeout
    if(DISPLAY_SLEEP_MS > 0 && !disp.isSleeping() && (now-lastActivityMs)>=DISPLAY_SLEEP_MS){
        disp.sleep();
    }

    // Display refresh + auto page flip
    if(now-lastDisplayMs>=OLED_REFRESH_MS){
        lastDisplayMs=now;
        MinerStats ms=miner.getStats();
        DisplayData dd={};
        dd.hash_rate       =isMaster?totalWorkerHR+ms.hash_rate:ms.hash_rate;
        dd.total_hashes    =ms.total_hashes;
        dd.found_blocks    =foundBlocks;
        // Accumulate accepted shares — persists across reconnects
        {
            static uint32_t lastAcc=0;
            uint32_t curAcc=stratum.acceptedShares();
            if(curAcc>lastAcc){ acceptedShares+=curAcc-lastAcc; lastAcc=curAcc; }
        }
        dd.accepted_shares = acceptedShares;
        dd.is_master       =isMaster;
        dd.uptime_s        =(now-startupMs)/1000;
        dd.peer_count      =mesh.peerCount();
        strncpy(dd.pool_host,POOL_HOST,sizeof(dd.pool_host)-1);
        dd.pool_port       =POOL_PORT;
        dd.difficulty      =isMaster?stratum.session().difficulty:0;
        if(WiFi.status()==WL_CONNECTED){
            strncpy(dd.ssid,WiFi.SSID().c_str(),sizeof(dd.ssid)-1);
            strncpy(dd.ip,WiFi.localIP().toString().c_str(),sizeof(dd.ip)-1);
            dd.rssi=WiFi.RSSI();
        }
        int cnt=min(mesh.peerCount(),8);
        for(int i=0;i<cnt;i++){
            PeerInfo p;if(mesh.getPeer(i,p)){memcpy(dd.worker_macs[i],p.mac,6);dd.worker_hash_rates[i]=p.hash_rate;}
        }
        if(strlen(lastStratumJob.job_id)) strncpy(dd.job_id,lastStratumJob.job_id,15);
        dd.mining_active = (ms.total_hashes > 0);
        dd.tick          = (uint32_t)(now / 500);

        // Auto page flip — advance page every PAGE_FLIP_MS
        if(!disp.isSleeping() && now-lastPageFlipMs>=PAGE_FLIP_MS){
            lastPageFlipMs=now;
            disp.nextPage();
            Serial.printf("[Display] auto -> page %d\n", (int)((now/PAGE_FLIP_MS)%PAGE_COUNT));
        }

        disp.update(dd);
    }

    // Blue LED blinks at hash speed — faster = more hashes
    // At 30kH/s: blink every ~33ms. Cap at 50ms min so it's visible.
    {
        uint32_t hr = miner.getStats().hash_rate;
        uint32_t blinkInterval = (hr > 0) ? max(50UL, 1000000UL/hr) : 500;
        if(now-lastLedMs >= blinkInterval){
            lastLedMs=now;
            ledState=!ledState;
            digitalWrite(LED_PIN, ledState ? HIGH : LOW);
        }
    }

    delay(10);
}
*/*
 * ═══════════════════════════════════════════════════════════════
 *  MeshMiner32 Edition  —  Single-file Arduino sketch
 *  Target  : ESP32 DevKit V1  (esp32 board package 3.x)
 *  Pool    : public-pool.io:3333  (0% fee, verify at web.public-pool.io)
 *  Display : SH1106 128x64 — 4-wire Hardware SPI
 *
 *  Libraries (install via Arduino Library Manager):
 *    ArduinoJson  >= 7.0   (bblanchon)
 *    U8g2         >= 2.35  (olikraus)
 *
 *  SPI OLED wiring (ESP32 DevKit V1):
 *  ┌─────────────┬───────────────────────────────┐
 *  │  OLED pin   │  ESP32 pin                    │
 *  ├─────────────┼───────────────────────────────┤
 *  │  VCC        │  3.3V                         │
 *  │  GND        │  GND                          │
 *  │  CLK / D0   │  GPIO 18  (HW SPI SCK)        │
 *  │  MOSI / D1  │  GPIO 23  (HW SPI MOSI)       │
 *  │  CS         │  GND  (CS tied low on module) │
 *  │  DC         │  GPIO 22                      │
 *  │  RST        │  GPIO  4                      │
 *  └─────────────┴───────────────────────────────┘
 *
 *  Monitor your miner at: https://web.public-pool.io
 *  Enter your BTC address to see hashrate & shares live.
 * ═══════════════════════════════════════════════════════════════
 */

// ───────────────────────────────────────────────────────────────
//  SECTION 1 — USER CONFIGURATION  (edit these before flashing)
// ───────────────────────────────────────────────────────────────

#define WIFI_SSID        "Home Assistant"
#define WIFI_PASSWORD    "7537819ajk"
#define WIFI_TIMEOUT_MS  20000

// public-pool.io — 0% fee solo pool, tracks workers by BTC address
// Append a worker name after a dot so it shows in the dashboard
// e.g.  bc1qXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX.esp32master
#define POOL_HOST   "public-pool.io"
#define POOL_PORT   3333
#define POOL_USER   "bc1qdh02k3mrznn038g62arfpe8n42nk9uc96hcpty.MeshMiner32"
#define POOL_PASS   "x"

#define STRATUM_TIMEOUT_MS  10000

// Node role:
//   ROLE_AUTO   = tries WiFi → if connected becomes master, else worker
//   ROLE_MASTER = always master (needs WiFi creds above)
//   ROLE_WORKER = always worker  (no WiFi needed)
#define ROLE_AUTO    0
#define ROLE_MASTER  1
#define ROLE_WORKER  2
#define NODE_ROLE    ROLE_AUTO

// ── SH1106 SPI pins ──────────────────────────────────────────
//  SCL (CLK)  -> GPIO 18  (ESP32 HW SPI SCK,  label D18)
//  SDA (MOSI) -> GPIO 23  (ESP32 HW SPI MOSI, label D23)
//  DC         -> GPIO 22  (label D22)
//  RES        -> GPIO  4  (label D4)
//  CS         -> GND on module — use U8X8_PIN_NONE in code
#define OLED_DC   22
#define OLED_RST  4

#define OLED_REFRESH_MS   1000
#define PAGE_FLIP_MS      3000   // auto-advance page every N ms
#define DISPLAY_SLEEP_MS  30000  // turn off OLED after 30s of inactivity (0 = never)
#define BOOT_BTN_PIN        0    // GPIO0 = BOOT button on DevKit V1

// ── Mining task ──────────────────────────────────────────────
#define MINING_TASK_PRIORITY  5
#define MINING_TASK_CORE      1      // core 1 — WiFi stays on core 0
#define MINING_TASK_STACK     12288

// ── ESP-NOW mesh ─────────────────────────────────────────────
#define ESPNOW_CHANNEL          1
#define PEER_TIMEOUT_MS         15000
#define HEARTBEAT_INTERVAL_MS   5000
#define ELECTION_TRIGGER_COUNT  3
#define ELECTION_BACKOFF_MAX_MS 2000
#define MESH_MAX_PEERS          20
#define MESH_FRAME_LEN          250

#define SERIAL_BAUD       115200
#define LED_PIN           2    // Built-in blue LED on ESP32 DevKit V1
#define POOL_RETRY_MS  15000   // Only retry pool connection every 15s (non-blocking)

// ───────────────────────────────────────────────────────────────
//  SECTION 2 — INCLUDES
// ───────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <functional>
#include <string.h>

// ───────────────────────────────────────────────────────────────
//  SECTION 3 — FORWARD DECLARATIONS
//  (prevents Arduino IDE preprocessor ordering issues)
// ───────────────────────────────────────────────────────────────

struct StratumJob;
struct StratumSession;
struct MinerJob;
struct MinerStats;
struct PeerInfo;
struct DisplayData;

static void dispatchJob(const StratumJob& sj);
static void checkElection();

// ───────────────────────────────────────────────────────────────
//  SECTION 4 — SHA-256  (double SHA for Bitcoin mining)
// ───────────────────────────────────────────────────────────────

static const uint32_t SHA_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static const uint32_t SHA_H0[8] = {
    0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
};

#define ROTR32(x,n)    (((x)>>(n))|((x)<<(32-(n))))
#define SHA_CH(e,f,g)  (((e)&(f))^(~(e)&(g)))
#define SHA_MAJ(a,b,c) (((a)&(b))^((a)&(c))^((b)&(c)))
#define SHA_EP0(a)     (ROTR32(a,2) ^ROTR32(a,13)^ROTR32(a,22))
#define SHA_EP1(e)     (ROTR32(e,6) ^ROTR32(e,11)^ROTR32(e,25))
#define SHA_SIG0(x)    (ROTR32(x,7) ^ROTR32(x,18)^((x)>>3))
#define SHA_SIG1(x)    (ROTR32(x,17)^ROTR32(x,19)^((x)>>10))

static inline uint32_t sha_be32(const uint8_t* p){
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static inline void sha_wr32(uint8_t* p,uint32_t v){
    p[0]=(v>>24)&0xFF;p[1]=(v>>16)&0xFF;p[2]=(v>>8)&0xFF;p[3]=v&0xFF;
}

static void sha256_compress(const uint32_t* in16,uint32_t* st){
    uint32_t w[64];
    for(int i=0;i<16;i++) w[i]=in16[i];
    for(int i=16;i<64;i++) w[i]=SHA_SIG1(w[i-2])+w[i-7]+SHA_SIG0(w[i-15])+w[i-16];
    uint32_t a=st[0],b=st[1],c=st[2],d=st[3],e=st[4],f=st[5],g=st[6],h=st[7];
    for(int i=0;i<64;i++){
        uint32_t t1=h+SHA_EP1(e)+SHA_CH(e,f,g)+SHA_K[i]+w[i];
        uint32_t t2=SHA_EP0(a)+SHA_MAJ(a,b,c);
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    st[0]+=a;st[1]+=b;st[2]+=c;st[3]+=d;
    st[4]+=e;st[5]+=f;st[6]+=g;st[7]+=h;
}

static void double_sha256(const uint8_t* data,size_t len,uint8_t* digest){
    uint8_t mid[32];
    {
        uint32_t st[8]; memcpy(st,SHA_H0,32);
        uint8_t buf[64];
        size_t rem=len; const uint8_t* ptr=data;
        while(rem>=64){
            uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(ptr+i*4);
            sha256_compress(w,st); ptr+=64; rem-=64;
        }
        memset(buf,0,64); memcpy(buf,ptr,rem); buf[rem]=0x80;
        if(rem>=56){
            uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(buf+i*4);
            sha256_compress(w,st); memset(buf,0,64);
        }
        uint64_t bits=(uint64_t)len*8;
        for(int i=0;i<8;i++) buf[56+i]=(bits>>((7-i)*8))&0xFF;
        uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(buf+i*4);
        sha256_compress(w,st);
        for(int i=0;i<8;i++) sha_wr32(mid+i*4,st[i]);
    }
    {
        uint32_t st[8]; memcpy(st,SHA_H0,32);
        uint8_t buf[64]={0}; memcpy(buf,mid,32); buf[32]=0x80;
        buf[62]=0x01; buf[63]=0x00;
        uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(buf+i*4);
        sha256_compress(w,st);
        for(int i=0;i<8;i++) sha_wr32(digest+i*4,st[i]);
    }
}

static void bitcoin_hash(const uint8_t* h80,uint8_t* o32){ double_sha256(h80,80,o32); }

static bool check_difficulty(const uint8_t* hash,const uint8_t* tgt){
    for(int i=31;i>=0;i--){
        if(hash[i]<tgt[i]) return true;
        if(hash[i]>tgt[i]) return false;
    }
    return true;
}

static void nbits_to_target(const uint8_t* nb,uint8_t* t32){
    memset(t32,0,32); uint8_t exp=nb[0]; if(exp==0||exp>32) return;
    int pos=(int)exp-1;
    if(pos>=0   &&pos<32) t32[pos]  =nb[1];
    if(pos-1>=0 &&pos-1<32) t32[pos-1]=nb[2];
    if(pos-2>=0 &&pos-2<32) t32[pos-2]=nb[3];
}

// ───────────────────────────────────────────────────────────────
//  SECTION 5 — HEX HELPERS
// ───────────────────────────────────────────────────────────────

static void hexToBytes(const char* hex,uint8_t* out,int maxLen){
    int len=(int)(strlen(hex)/2); if(len>maxLen) len=maxLen;
    for(int i=0;i<len;i++){
        auto nib=[](char c)->uint8_t{
            if(c>='0'&&c<='9') return c-'0';
            if(c>='a'&&c<='f') return c-'a'+10;
            if(c>='A'&&c<='F') return c-'A'+10;
            return 0;
        };
        out[i]=(nib(hex[i*2])<<4)|nib(hex[i*2+1]);
    }
}

static String bytesToHex(const uint8_t* b,int len){
    static const char* H="0123456789abcdef";
    String s; s.reserve(len*2);
    for(int i=0;i<len;i++){s+=H[(b[i]>>4)&0xF];s+=H[b[i]&0xF];}
    return s;
}

// ───────────────────────────────────────────────────────────────
//  SECTION 6 — STRATUM STRUCTS
// ───────────────────────────────────────────────────────────────

struct StratumJob {
    char     job_id[64];
    uint8_t  prev_hash[32];
    uint8_t  coinbase1[128]; uint16_t coinbase1_len;
    uint8_t  coinbase2[128]; uint16_t coinbase2_len;
    uint8_t  merkle_branches[16][32];
    uint8_t  merkle_count;
    uint8_t  version[4];
    uint8_t  nbits[4];
    uint8_t  ntime[4];
    bool     clean_jobs;
    bool     valid;
};

struct StratumSession {
    uint8_t  extranonce1[8];
    uint8_t  extranonce1_len;
    uint8_t  extranonce2_len;
    uint32_t difficulty;
    bool     subscribed;
    bool     authorized;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 7 — ESP-NOW MESH STRUCTS
// ───────────────────────────────────────────────────────────────

#define MSG_JOB        0x01
#define MSG_RESULT     0x02
#define MSG_STATS      0x03
#define MSG_HEARTBEAT  0x04
#define MSG_ELECT      0x05

static const uint8_t ESPNOW_BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

#pragma pack(push,1)
typedef struct {
    uint8_t  msg_type;
    uint8_t  job_id;
    uint8_t  version[4];
    uint8_t  prev_hash[32];
    uint8_t  merkle_root[32];
    uint8_t  nbits[4];
    uint8_t  ntime[4];
    uint32_t nonce_start;
    uint32_t nonce_end;
    uint8_t  extranonce2[8];
    uint8_t  extranonce2_len;
    uint8_t  assigned_chunk;
} MeshJobMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  job_id;
    uint32_t nonce;
    uint8_t  worker_mac[6];
} MeshResultMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  worker_mac[6];
    uint32_t hash_rate;
    uint32_t nonces_tested;
    uint8_t  job_id;
} MeshStatsMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  sender_mac[6];
    uint8_t  is_master;
    uint32_t uptime_s;
    uint32_t total_hash_rate;
} MeshHeartbeatMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  candidate_mac[6];
    uint8_t  priority;
} MeshElectMsg;
#pragma pack(pop)

struct PeerInfo {
    uint8_t  mac[6];
    bool     active;
    uint32_t last_seen_ms;
    uint32_t hash_rate;
    uint8_t  assigned_chunk;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 8 — EspNowMesh CLASS
// ───────────────────────────────────────────────────────────────

class EspNowMesh {
public:
    static EspNowMesh& instance(){ static EspNowMesh i; return i; }

    bool begin(bool isMaster, uint8_t channel=ESPNOW_CHANNEL){
        _isMaster=isMaster;
        _channel=channel;
        if(isMaster) WiFi.mode(WIFI_AP_STA);
        else       { WiFi.mode(WIFI_STA); WiFi.disconnect(); }
        // Force ESP-NOW onto the same channel WiFi is using
        esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
        Serial.printf("[Mesh] ESP-NOW channel: %d\n",(int)_channel);
        if(esp_now_init()!=ESP_OK){ Serial.println("[Mesh] init failed"); return false; }
        esp_now_register_recv_cb(_recv_cb);
        esp_now_register_send_cb(_send_cb);
        esp_now_peer_info_t bc={};
        memcpy(bc.peer_addr,ESPNOW_BROADCAST,6);
        bc.channel=_channel; bc.encrypt=false;
        if(esp_now_add_peer(&bc)!=ESP_OK){ Serial.println("[Mesh] bcast fail"); return false; }
        _initialized=true;
        Serial.printf("[Mesh] ready as %s\n",isMaster?"MASTER":"WORKER");
        return true;
    }

    bool broadcastJob(const MeshJobMsg& j)                  { return _tx(ESPNOW_BROADCAST,&j,sizeof(j)); }
    bool sendResult(const uint8_t* m,const MeshResultMsg& r){ return _tx(m,&r,sizeof(r)); }
    bool sendStats(const uint8_t* m,const MeshStatsMsg& s)  { return _tx(m,&s,sizeof(s)); }
    bool broadcastHeartbeat(const MeshHeartbeatMsg& h)       { return _tx(ESPNOW_BROADCAST,&h,sizeof(h)); }
    bool broadcastElection(const MeshElectMsg& e)            { return _tx(ESPNOW_BROADCAST,&e,sizeof(e)); }

    bool addPeer(const uint8_t* mac){
        bool bc=true; for(int i=0;i<6;i++) if(mac[i]!=0xFF){bc=false;break;} if(bc) return true;
        for(int i=0;i<_peerCount;i++) if(memcmp(_peers[i].mac,mac,6)==0) return true;
        if(_peerCount>=MESH_MAX_PEERS) return false;
        if(!esp_now_is_peer_exist(mac)){
            esp_now_peer_info_t pi={}; memcpy(pi.peer_addr,mac,6);
            pi.channel=_channel; pi.encrypt=false;
            if(esp_now_add_peer(&pi)!=ESP_OK) return false;
        }
        PeerInfo& p=_peers[_peerCount++];
        memcpy(p.mac,mac,6); p.active=true; p.last_seen_ms=millis(); p.hash_rate=0; p.assigned_chunk=0;
        Serial.printf("[Mesh] +peer %02X:%02X:%02X:%02X:%02X:%02X\n",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        _newPeer=true;   // signal main loop to redispatch
        return true;
    }

    void removeStalePeers(uint32_t now){
        for(int i=0;i<_peerCount;i++)
            if(_peers[i].active&&(now-_peers[i].last_seen_ms)>PEER_TIMEOUT_MS)
                _peers[i].active=false;
    }

    int  peerCount() const { int c=0; for(int i=0;i<_peerCount;i++) if(_peers[i].active) c++; return c; }

    bool getPeer(int idx,PeerInfo& out) const {
        int a=0;
        for(int i=0;i<_peerCount;i++) if(_peers[i].active){ if(a==idx){out=_peers[i];return true;} a++; }
        return false;
    }

    void updatePeerStats(const uint8_t* mac,uint32_t hr,uint32_t now){
        for(int i=0;i<_peerCount;i++) if(memcmp(_peers[i].mac,mac,6)==0){
            _peers[i].hash_rate=hr; _peers[i].last_seen_ms=now; _peers[i].active=true; return;
        }
        addPeer(mac); updatePeerStats(mac,hr,now);
    }

    void setMasterMac(const uint8_t* mac){ memcpy(_masterMac,mac,6); _hasMaster=true; addPeer(mac); }
    bool getMasterMac(uint8_t* out) const { if(!_hasMaster) return false; memcpy(out,_masterMac,6); return true; }
    bool hasMaster() const { return _hasMaster; }

    std::function<void(const MeshJobMsg&)>        onJob;
    std::function<void(const MeshResultMsg&)>     onResult;
    std::function<void(const MeshStatsMsg&)>      onStats;
    std::function<void(const MeshHeartbeatMsg&)>  onHeartbeat;
    std::function<void(const MeshElectMsg&)>      onElect;

    // Check and clear the new-peer flag (set by ISR when a worker is first seen)
    bool takeNewPeer(){ if(_newPeer){_newPeer=false;return true;} return false; }

private:
    EspNowMesh()=default;

    bool _tx(const uint8_t* mac,const void* data,size_t len){
        if(!_initialized||len>MESH_FRAME_LEN) return false;
        // Re-sync all peer channel registrations if WiFi channel has drifted.
        // The AP can change channels at any time; stale peer channels cause every send to fail.
        uint8_t curCh; wifi_second_chan_t sec;
        if(esp_wifi_get_channel(&curCh,&sec)==ESP_OK && curCh!=0 && curCh!=_channel){
            Serial.printf("[Mesh] channel drift %d->%d, resyncing peers\n",(int)_channel,(int)curCh);
            _channel=curCh;
            esp_wifi_set_channel(_channel,WIFI_SECOND_CHAN_NONE);
            esp_now_peer_info_t pi={}; pi.channel=_channel; pi.encrypt=false;
            // Update broadcast peer
            memcpy(pi.peer_addr,ESPNOW_BROADCAST,6);
            esp_now_mod_peer(&pi);
            // Update all unicast peers
            for(int i=0;i<_peerCount;i++){
                memcpy(pi.peer_addr,_peers[i].mac,6);
                esp_now_mod_peer(&pi);
            }
        }
        uint8_t frame[MESH_FRAME_LEN]={0}; memcpy(frame,data,len);
        return esp_now_send(mac,frame,MESH_FRAME_LEN)==ESP_OK;
    }

    // ESP32 core 3.x callback signatures
    static void _recv_cb(const esp_now_recv_info_t* info,const uint8_t* data,int len){
        if(len<1||!info) return;
        const uint8_t* mac=info->src_addr;
        EspNowMesh& m=instance(); m.addPeer(mac);
        uint8_t type=data[0];
        switch(type){
            case MSG_JOB:       if(len>=(int)sizeof(MeshJobMsg))      {MeshJobMsg       x;memcpy(&x,data,sizeof(x));if(m.onJob)      m.onJob(x);}      break;
            case MSG_RESULT:    if(len>=(int)sizeof(MeshResultMsg))   {MeshResultMsg    x;memcpy(&x,data,sizeof(x));if(m.onResult)   m.onResult(x);}   break;
            case MSG_STATS:     if(len>=(int)sizeof(MeshStatsMsg))    {MeshStatsMsg     x;memcpy(&x,data,sizeof(x));m.updatePeerStats(mac,x.hash_rate,millis());if(m.onStats)m.onStats(x);} break;
            case MSG_HEARTBEAT: if(len>=(int)sizeof(MeshHeartbeatMsg)){MeshHeartbeatMsg x;memcpy(&x,data,sizeof(x));
                                    if(x.is_master&&!m._hasMaster){m.setMasterMac(mac);Serial.printf("[Mesh] master %02X:%02X:%02X\n",mac[0],mac[1],mac[2]);}
                                    m.updatePeerStats(mac,x.total_hash_rate,millis());if(m.onHeartbeat)m.onHeartbeat(x);} break;
            case MSG_ELECT:     if(len>=(int)sizeof(MeshElectMsg))    {MeshElectMsg     x;memcpy(&x,data,sizeof(x));if(m.onElect)    m.onElect(x);}    break;
            default: break;
        }
    }
    static void _send_cb(const wifi_tx_info_t*,esp_now_send_status_t){}

    PeerInfo _peers[MESH_MAX_PEERS];
    int      _peerCount   = 0;
    bool     _initialized = false;
    bool     _isMaster    = false;
    bool     _hasMaster   = false;
    uint8_t  _masterMac[6]= {0};
    uint8_t  _channel     = ESPNOW_CHANNEL;
    volatile bool _newPeer = false;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 9 — STRATUM CLIENT
// ───────────────────────────────────────────────────────────────

class StratumClient {
public:
    bool connect(const char* host,uint16_t port,const char* user,const char* pass){
        Serial.printf("[Stratum] -> %s:%d\n",host,port);
        if(!_client.connect(host,port)){Serial.println("[Stratum] connection failed");return false;}
        _lastAct=millis();
        _tx("{\"id\":"+String(_id++)+",\"method\":\"mining.subscribe\",\"params\":[\"MeshMiner32/1.0\",null]}\n");
        uint32_t t=millis();
        while(millis()-t<STRATUM_TIMEOUT_MS){
            String line; if(_rxLine(line,500)){
                JsonDocument d;
                if(deserializeJson(d,line)==DeserializationError::Ok) _parseSub(d);
                if(_s.subscribed) break;
            }
            delay(10);
        }
        if(!_s.subscribed){Serial.println("[Stratum] subscribe timeout");return false;}
        _tx("{\"id\":"+String(_id++)+",\"method\":\"mining.authorize\",\"params\":[\""+String(user)+"\",\""+String(pass)+"\"]}\n");
        Serial.println("[Stratum] authorized, waiting for job...");
        return true;
    }

    void disconnect(){ _client.stop(); }
    bool isConnected(){ return _client.connected(); }

    void loop(){
        if(!_client.connected()) return;
        if(millis()-_lastAct>30000){
            _tx("{\"id\":"+String(_id++)+",\"method\":\"mining.ping\",\"params\":[]}\n");
            _lastAct=millis();
        }
        while(_client.available()){
            char c=_client.read();
            if(c=='\n'){if(_buf.length()>0)_procLine(_buf);_buf="";}
            else _buf+=c;
            _lastAct=millis();
        }
    }

    bool submit(const StratumJob& job,uint32_t nonce,const uint8_t* en2,uint8_t en2len){
        String msg="{\"id\":"+String(_id++)+",\"method\":\"mining.submit\",\"params\":[\""
            +String(POOL_USER)+"\",\""+String(job.job_id)+"\",\""
            +bytesToHex(en2,en2len)+"\",\""+bytesToHex(job.ntime,4)+"\",\""
            +bytesToHex((const uint8_t*)&nonce,4)+"\"]}\n";
        _submits++; return _tx(msg);
    }

    void computeMerkleRoot(uint8_t* root,const uint8_t* coinbase,uint16_t cbLen){
        double_sha256(coinbase,cbLen,root);
        for(int i=0;i<_job.merkle_count;i++){
            uint8_t pair[64]; memcpy(pair,root,32); memcpy(pair+32,_job.merkle_branches[i],32);
            double_sha256(pair,64,root);
        }
    }

    std::function<void(const StratumJob&)> onJob;

    const StratumJob&     currentJob()     const { return _job; }
    const StratumSession& session()        const { return _s; }
    uint32_t              acceptedShares() const { return _accepted; }
    uint32_t              totalSubmits()   const { return _submits; }

private:
    bool _tx(const String& msg){ if(!_client.connected()) return false; _client.print(msg); return true; }

    bool _rxLine(String& out,uint32_t tms){
        uint32_t t=millis();
        while(millis()-t<tms){if(_client.available()){out=_client.readStringUntil('\n');return out.length()>0;}delay(5);}
        return false;
    }

    void _procLine(const String& line){
        JsonDocument doc;
        if(deserializeJson(doc,line)!=DeserializationError::Ok) return;
        const char* method=doc["method"];
        if(method){
            if(strcmp(method,"mining.notify")==0)              _parseNotify(doc);
            else if(strcmp(method,"mining.set_difficulty")==0) _parseDiff(doc);
        } else {
            if((doc["result"]|false)&&(doc["id"]|0)>0) _accepted++;
        }
    }

    void _parseNotify(const JsonDocument& doc){
        JsonArrayConst p=doc["params"];
        if(p.isNull()||p.size()<9) return;
        StratumJob j={};
        strncpy(j.job_id,p[0]|"",sizeof(j.job_id)-1);
        hexToBytes(p[1]|"",j.prev_hash,32);
        j.coinbase1_len=strlen(p[2]|"")/2; hexToBytes(p[2]|"",j.coinbase1,sizeof(j.coinbase1));
        j.coinbase2_len=strlen(p[3]|"")/2; hexToBytes(p[3]|"",j.coinbase2,sizeof(j.coinbase2));
        JsonArrayConst br=p[4];
        j.merkle_count=min((int)br.size(),16);
        for(int i=0;i<j.merkle_count;i++) hexToBytes(br[i]|"",j.merkle_branches[i],32);
        hexToBytes(p[5]|"",j.version,4);
        hexToBytes(p[6]|"",j.nbits,4);
        hexToBytes(p[7]|"",j.ntime,4);
        j.clean_jobs=p[8]|false; j.valid=true;
        _job=j;
        Serial.printf("[Stratum] job %s  clean=%d\n",j.job_id,(int)j.clean_jobs);
        if(onJob) onJob(_job);
    }

    void _parseDiff(const JsonDocument& doc){
        JsonArrayConst p=doc["params"]; if(p.isNull()) return;
        _s.difficulty=p[0]|1;
        Serial.printf("[Stratum] difficulty %u\n",_s.difficulty);
    }

    void _parseSub(const JsonDocument& doc){
        JsonVariantConst r=doc["result"]; if(r.isNull()) return;
        const char* en1=r[1]|"";
        _s.extranonce1_len=min((int)(strlen(en1)/2),8);
        hexToBytes(en1,_s.extranonce1,_s.extranonce1_len);
        _s.extranonce2_len=r[2]|4;
        _s.subscribed=true;
        Serial.printf("[Stratum] subscribed  en1=%d  en2_size=%d\n",_s.extranonce1_len,_s.extranonce2_len);
    }

    WiFiClient     _client;
    StratumJob     _job    = {};
    StratumSession _s      = {};
    int            _id     = 1;
    uint32_t       _submits  = 0;
    uint32_t       _accepted = 0;
    uint32_t       _lastAct  = 0;
    String         _buf;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 10 — MINER  (FreeRTOS task, Core 1)
// ───────────────────────────────────────────────────────────────

struct MinerJob {
    uint8_t  header[80];
    uint8_t  target[32];
    uint32_t nonce_start;
    uint32_t nonce_end;
    uint8_t  job_id;
    uint8_t  stratum_job_id[64];
    uint8_t  extranonce2[8];
    uint8_t  extranonce2_len;
    bool     valid;
};

struct MinerStats {
    uint32_t hash_rate;
    uint32_t total_hashes;
    uint32_t found_nonces;
    uint32_t last_update_ms;
};

class Miner {
public:
    static Miner& instance(){ static Miner m; return m; }

    void begin(){
        _mutex=xSemaphoreCreateMutex();
        xTaskCreatePinnedToCore(_task,"mining",MINING_TASK_STACK,this,MINING_TASK_PRIORITY,nullptr,MINING_TASK_CORE);
        Serial.printf("[Miner] task on core %d\n",MINING_TASK_CORE);
    }

    void setJob(const MinerJob& j){
        if(!_mutex) return;
        xSemaphoreTake(_mutex,portMAX_DELAY); _pending=j; _newJob=true; xSemaphoreGive(_mutex);
    }

    MinerStats getStats() const { return _stats; }

    std::function<void(const MinerJob&,uint32_t)> onFound;

private:
    Miner()=default;
    static void _task(void* p){ static_cast<Miner*>(p)->_run(); vTaskDelete(nullptr); }

    void _run(){
        uint8_t hash[32];
        uint32_t cnt=0,wstart=millis();
        while(true){
            if(_newJob&&_mutex&&xSemaphoreTake(_mutex,0)==pdTRUE){
                _job=_pending; _newJob=false; cnt=0; wstart=millis(); xSemaphoreGive(_mutex);
                Serial.printf("[Miner] job=%d  %08X-%08X\n",_job.job_id,_job.nonce_start,_job.nonce_end);
            }
            if(!_job.valid){ vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            for(uint32_t n=_job.nonce_start;n<_job.nonce_end&&!_newJob;n++){
                _job.header[76]= n     &0xFF;
                _job.header[77]=(n>> 8)&0xFF;
                _job.header[78]=(n>>16)&0xFF;
                _job.header[79]=(n>>24)&0xFF;
                bitcoin_hash(_job.header,hash);
                cnt++;
                if(check_difficulty(hash,_job.target)){
                    _stats.found_nonces++;
                    Serial.printf("[Miner] *** FOUND nonce=%08X ***\n",n);
                    if(onFound) onFound(_job,n);
                }
                if((cnt&0x3FF)==0){
                    uint32_t el=millis()-wstart;
                    if(el>0) _stats.hash_rate=(cnt*1000UL)/el;
                    _stats.total_hashes+=1024;
                    _stats.last_update_ms=millis();
                    // vTaskDelay(1) feeds the watchdog AND yields properly
                    vTaskDelay(1);
                }
            }
            if(!_newJob) _job.valid=false;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    volatile bool     _newJob  = false;
    MinerJob          _job     = {};
    MinerJob          _pending = {};
    MinerStats        _stats   = {};
    SemaphoreHandle_t _mutex   = nullptr;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 11 — SH1106 SPI OLED DISPLAY  (U8g2 HW SPI)
// ───────────────────────────────────────────────────────────────

enum DisplayPage { PAGE_MINING=0,PAGE_POOL,PAGE_NETWORK,PAGE_MESH,PAGE_COUNT };

struct DisplayData {
    uint32_t hash_rate,total_hashes,found_blocks,accepted_shares;
    char     job_id[16];
    char     pool_host[32]; uint16_t pool_port; uint32_t difficulty;
    char     ssid[32]; int8_t rssi; char ip[16];
    int      peer_count;
    uint32_t worker_hash_rates[8];
    uint8_t  worker_macs[8][6];
    bool     is_master;
    uint32_t uptime_s;
    bool     mining_active;  // true once miner has processed at least one batch
    uint32_t tick;           // increments each display refresh — drives animations
};

class OledDisplay {
public:
    static OledDisplay& instance(){ static OledDisplay o; return o; }

    void begin(){
        _u8g2.begin();
        _u8g2.setContrast(200);
        _on=true;
        _splash();
    }

    void showStatus(const char* l1,const char* l2=nullptr){
        if(!_on) return;
        _u8g2.clearBuffer();
        _u8g2.setFont(u8g2_font_6x10_tf);
        _u8g2.drawStr(0,14,l1);
        if(l2) _u8g2.drawStr(0,28,l2);
        _u8g2.sendBuffer();
    }

    void nextPage(){ _page=(DisplayPage)(((int)_page+1)%PAGE_COUNT); }
    void setPage(DisplayPage p){ _page=p; }

    void sleep(){
        if(_sleeping) return;
        _sleeping=true;
        _u8g2.setPowerSave(1);
        Serial.println("[Display] sleep");
    }

    void wake(){
        if(!_sleeping) return;
        _sleeping=false;
        _u8g2.setPowerSave(0);
        Serial.println("[Display] wake");
    }

    bool isSleeping() const { return _sleeping; }

    void update(const DisplayData& d){
        if(!_on||_sleeping) return;
        _u8g2.clearBuffer();
        switch(_page){
            case PAGE_MINING:  _drawMining(d);  break;
            case PAGE_POOL:    _drawPool(d);    break;
            case PAGE_NETWORK: _drawNet(d);     break;
            case PAGE_MESH:    _drawMesh(d);    break;
            default: break;
        }
        // page indicator dots — bottom right
        for(int i=0;i<PAGE_COUNT;i++){
            if(i==(int)_page) _u8g2.drawBox(122-(PAGE_COUNT-1-i)*6,61,4,3);
            else              _u8g2.drawFrame(122-(PAGE_COUNT-1-i)*6,61,4,3);
        }
        _u8g2.sendBuffer();
    }

private:
    OledDisplay()=default;

    // SPI OLED with no CS pin (CS tied to GND on module).
    // U8G2_SH1106 4W_HW_SPI: (rotation, cs, dc, reset)
    // Pass U8X8_PIN_NONE for cs — library skips the CS toggle.
    U8G2_SH1106_128X64_NONAME_F_4W_HW_SPI _u8g2{
        U8G2_R0,
        /* cs=   */ U8X8_PIN_NONE,
        /* dc=   */ OLED_DC,
        /* reset=*/ OLED_RST
    };

    DisplayPage _page    = PAGE_MINING;
    bool        _on      = false;
    bool        _sleeping= false;

    void _splash(){
        _u8g2.clearBuffer();
        // ── Bitcoin ₿ symbol drawn with primitives ──
        // Outer circle
        _u8g2.drawCircle(32,26,18);
        // Vertical stem top
        _u8g2.drawVLine(32,7,4);
        // Vertical stem bottom
        _u8g2.drawVLine(32,41,4);
        // Left vertical bar of B
        _u8g2.drawVLine(27,14,24);
        _u8g2.drawVLine(28,14,24);
        // Top bump of B
        _u8g2.drawHLine(28,14,8);
        _u8g2.drawHLine(28,23,8);
        _u8g2.drawVLine(36,14,9);
        // Bottom bump of B (wider)
        _u8g2.drawHLine(28,24,9);
        _u8g2.drawHLine(28,38,9);
        _u8g2.drawVLine(37,24,14);
        // Slight tilt lines for $ style
        _u8g2.drawVLine(31,7,4);
        _u8g2.drawVLine(31,41,4);
        // ── Name — right half of screen (x=56 to 128 = 72px wide) ──
        // u8g2_font_9x15_tf: each char ~9px wide
        // "MeshMiner" = 9 chars = ~63px  fits in 72px
        // "32"        = 2 chars = ~18px
        _u8g2.setFont(u8g2_font_9x15_tf);
        _u8g2.drawStr(57,26,"MeshMiner");
        _u8g2.drawStr(57,44,"32");
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(57,57,"public-pool.io");
        _u8g2.sendBuffer();
    }

    static String _fmtHR(uint32_t h){
        if(h>=1000000) return String(h/1000000.0f,1)+"MH/s";
        if(h>=1000)    return String(h/1000.0f,1)+"kH/s";
        return String(h)+"H/s";
    }
    static String _fmtUp(uint32_t s){
        if(s<60)   return String(s)+"s";
        if(s<3600) return String(s/60)+"m";
        return String(s/3600)+"h";
    }

    void _drawMining(const DisplayData& d){
        // ── Header: name + spinner + uptime — all one line ──
        _u8g2.setFont(u8g2_font_5x7_tf);
        // Spinner indicator (| / - \) when mining, * when idle
        const char* spin[]={"| ","/ ","- ","\\ "};
        const char* ind = d.mining_active ? spin[d.tick%4] : "* ";
        char hdr[28];
        snprintf(hdr,sizeof(hdr),"%sMeshMiner32",ind);
        _u8g2.drawStr(0,7,hdr);
        char upbuf[12]; snprintf(upbuf,12,"UP:%s",_fmtUp(d.uptime_s).c_str());
        _u8g2.drawStr(90,7,upbuf);
        _u8g2.drawHLine(0,9,128);

        // ── Hashrate (big font) ─────────────────────
        _u8g2.setFont(u8g2_font_logisoso16_tr);
        String hr=d.mining_active?_fmtHR(d.hash_rate):"--";
        _u8g2.drawStr(0,30,hr.c_str());

        // ── Stats rows ──────────────────────────────
        _u8g2.setFont(u8g2_font_5x7_tf);
        char p1[18]; snprintf(p1,18,"Peers:%-2d",d.peer_count);
        _u8g2.drawStr(0,44,p1);
        char p2[18]; snprintf(p2,18,"Acc:%-5lu",(unsigned long)d.accepted_shares);
        _u8g2.drawStr(66,44,p2);
        char p3[18]; snprintf(p3,18,"Blk:%-3lu",(unsigned long)d.found_blocks);
        _u8g2.drawStr(0,55,p3);
        if(strlen(d.job_id)){
            char jbuf[12]; snprintf(jbuf,12,"J:%.6s",d.job_id);
            _u8g2.drawStr(66,55,jbuf);
        }
    }

    void _drawPool(const DisplayData& d){
        // Inverted header bar
        _u8g2.drawBox(0,0,128,10);
        _u8g2.setColorIndex(0);
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(2,8,"POOL");
        _u8g2.setColorIndex(1);
        _u8g2.setFont(u8g2_font_6x10_tf);
        char h[22]; snprintf(h,sizeof(h),"%.21s",d.pool_host);
        _u8g2.drawStr(0,22,h);
        _u8g2.drawStr(0,34,("Port: "+String(d.pool_port)).c_str());
        _u8g2.drawStr(0,46,("Diff: "+String(d.difficulty)).c_str());
        _u8g2.drawStr(0,58,("Acc: "+String(d.accepted_shares)).c_str());
    }

    void _drawNet(const DisplayData& d){
        _u8g2.drawBox(0,0,128,10);
        _u8g2.setColorIndex(0);
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(2,8,"NETWORK");
        _u8g2.setColorIndex(1);
        _u8g2.setFont(u8g2_font_6x10_tf);
        char s[22]; snprintf(s,sizeof(s),"%.21s",d.ssid); _u8g2.drawStr(0,22,s);
        _u8g2.drawStr(0,34,d.ip);
        _u8g2.drawStr(0,46,("RSSI: "+String(d.rssi)+"dBm").c_str());
        _u8g2.drawStr(0,58,("Mesh: "+String(d.peer_count)+" nodes").c_str());
    }

    void _drawMesh(const DisplayData& d){
        _u8g2.drawBox(0,0,128,10);
        _u8g2.setColorIndex(0);
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(2,8,"MESH WORKERS");
        _u8g2.setColorIndex(1);
        int cnt=min(d.peer_count,6);
        for(int i=0;i<cnt;i++){
            char line[28];
            snprintf(line,sizeof(line),"%02X:%02X:%02X %s",
                d.worker_macs[i][3],d.worker_macs[i][4],d.worker_macs[i][5],
                _fmtHR(d.worker_hash_rates[i]).c_str());
            _u8g2.drawStr(0,20+i*9,line);
        }
        if(d.peer_count==0){
            _u8g2.setFont(u8g2_font_6x10_tf);
            _u8g2.drawStr(10,38,"No workers");
            _u8g2.drawStr(10,52,"yet");
        }
    }
};

// ───────────────────────────────────────────────────────────────
//  SECTION 12 — GLOBAL STATE
// ───────────────────────────────────────────────────────────────

static EspNowMesh&   mesh    = EspNowMesh::instance();
static Miner&        miner   = Miner::instance();
static OledDisplay&  disp    = OledDisplay::instance();
static StratumClient stratum;

static bool     isMaster          = false;
static uint8_t  myMac[6]          = {0};
static uint8_t  currentJobId      = 0;
static uint32_t totalWorkerHR     = 0;
static uint32_t foundBlocks       = 0;
static uint32_t acceptedShares    = 0;   // persistent across pool reconnects
static uint32_t startupMs         = 0;
static uint32_t lastHeartbeatMs   = 0;
static uint32_t lastDisplayMs     = 0;
static uint32_t lastStatsMs       = 0;
static uint32_t masterLastSeenMs  = 0;
static bool     electionInProgress= false;
static uint32_t en2Counter        = 0;
// Cross-core nonce submission (miner runs Core 1, WiFi/ESP-NOW on Core 0)
static volatile bool     pendingNonceReady = false;
static volatile uint32_t pendingNonce      = 0;
static volatile uint8_t  pendingNonceJob   = 0;
static StratumJob lastStratumJob  = {};
static int      lastPeerCount     = 0;      // detect new worker joins
static uint32_t lastRebroadcastMs = 0;      // periodic job re-send to workers
static bool     _pendingRetry     = false;  // second dispatch ~800 ms after first
static uint32_t _retryMs          = 0;
static uint32_t lastPageFlipMs    = 0;      // auto page cycling
static uint32_t lastPoolRetryMs   = 0;      // non-blocking pool reconnect timer
static uint32_t lastLedMs         = 0;      // LED blink timer
static bool     ledState          = false;
static uint32_t lastBtnMs         = 0;      // button debounce
static bool     lastBtnState      = true;   // INPUT_PULLUP -> idle=HIGH
static uint32_t lastActivityMs    = 0;      // display sleep timeout

// ───────────────────────────────────────────────────────────────
//  SECTION 13 — JOB DISPATCH  (after all types are complete)
// ───────────────────────────────────────────────────────────────

static void buildCoinbase(uint8_t* out,uint16_t& outLen,
                           const StratumJob& sj,const StratumSession& sess,
                           uint32_t en2cnt)
{
    uint16_t pos=0;
    memcpy(out+pos,sj.coinbase1,sj.coinbase1_len); pos+=sj.coinbase1_len;
    memcpy(out+pos,sess.extranonce1,sess.extranonce1_len); pos+=sess.extranonce1_len;
    for(int i=0;i<sess.extranonce2_len&&i<4;i++) out[pos++]=(en2cnt>>(i*8))&0xFF;
    memcpy(out+pos,sj.coinbase2,sj.coinbase2_len); pos+=sj.coinbase2_len;
    outLen=pos;
}

static void dispatchJob(const StratumJob& sj){
    lastStratumJob=sj;
    en2Counter++;
    const StratumSession& sess=stratum.session();

    static uint8_t coinbase[300]; uint16_t cbLen=0;  // static avoids stack overflow
    buildCoinbase(coinbase,cbLen,sj,sess,en2Counter);
    uint8_t merkleRoot[32];
    stratum.computeMerkleRoot(merkleRoot,coinbase,cbLen);

    int workers=mesh.peerCount();
    int slots=workers+1;
    // Use 64-bit divide so result is correct for all slot counts (avoids overflow)
    uint32_t chunk=(uint32_t)(0x100000000ULL/(uint64_t)slots);
    if(chunk==0) chunk=0xFFFFFFFFUL;  // safety fallback

    // Build and broadcast job to each worker with its own nonce slice
    MeshJobMsg jm={};
    jm.msg_type=MSG_JOB; jm.job_id=++currentJobId;
    memcpy(jm.version,   sj.version,  4);
    memcpy(jm.prev_hash, sj.prev_hash,32);
    memcpy(jm.merkle_root,merkleRoot, 32);
    memcpy(jm.nbits,     sj.nbits,   4);
    memcpy(jm.ntime,     sj.ntime,   4);
    jm.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) jm.extranonce2[i]=(en2Counter>>(i*8))&0xFF;

    for(int i=0;i<workers;i++){
        PeerInfo peer; if(!mesh.getPeer(i,peer)) continue;
        jm.nonce_start=chunk*(uint32_t)(i+1);
        jm.nonce_end  =jm.nonce_start+chunk-1;
        jm.assigned_chunk=(uint8_t)(i+1);
        mesh.broadcastJob(jm);
    }

    // Master hashes chunk 0
    MinerJob mj={};
    mj.valid=true; mj.job_id=currentJobId;
    strncpy((char*)mj.stratum_job_id,sj.job_id,63);
    mj.nonce_start=0; mj.nonce_end=chunk-1;
    mj.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) mj.extranonce2[i]=(en2Counter>>(i*8))&0xFF;
    nbits_to_target(sj.nbits,mj.target);
    memcpy(mj.header,    sj.version,  4);
    memcpy(mj.header+4,  sj.prev_hash,32);
    memcpy(mj.header+36, merkleRoot,  32);
    memcpy(mj.header+68, sj.ntime,   4);
    memcpy(mj.header+72, sj.nbits,   4);
    miner.setJob(mj);

    Serial.printf("[Dispatch] job=%d  workers=%d  chunk=%08X\n",currentJobId,workers,chunk);
}

// Re-dispatch the current job to workers with fresh nonce slices.
// Called when a new worker joins or on the rebroadcast timer.
static void redispatchToWorkers(){
    if(!lastStratumJob.valid) return;
    int workers=mesh.peerCount();
    if(workers==0) return;
    int slots=workers+1;
    uint32_t chunk=(uint32_t)(0x100000000ULL/(uint64_t)slots);
    if(chunk==0) chunk=0xFFFFFFFFUL;

    MeshJobMsg jm={};
    jm.msg_type=MSG_JOB; jm.job_id=currentJobId;
    memcpy(jm.version,    lastStratumJob.version,  4);
    memcpy(jm.prev_hash,  lastStratumJob.prev_hash,32);
    const StratumSession& sess=stratum.session();
    static uint8_t coinbase[300]; uint16_t cbLen=0;  // static avoids stack overflow
    // reuse same en2Counter so workers hash complementary ranges
    uint16_t pos=0;
    memcpy(coinbase+pos,lastStratumJob.coinbase1,lastStratumJob.coinbase1_len); pos+=lastStratumJob.coinbase1_len;
    memcpy(coinbase+pos,sess.extranonce1,sess.extranonce1_len); pos+=sess.extranonce1_len;
    for(int i=0;i<sess.extranonce2_len&&i<4;i++) coinbase[pos++]=(en2Counter>>(i*8))&0xFF;
    memcpy(coinbase+pos,lastStratumJob.coinbase2,lastStratumJob.coinbase2_len); pos+=lastStratumJob.coinbase2_len;
    cbLen=pos;
    uint8_t merkleRoot[32];
    stratum.computeMerkleRoot(merkleRoot,coinbase,cbLen);

    memcpy(jm.merkle_root,merkleRoot,32);
    memcpy(jm.nbits,lastStratumJob.nbits,4);
    memcpy(jm.ntime,lastStratumJob.ntime,4);
    jm.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) jm.extranonce2[i]=(en2Counter>>(i*8))&0xFF;

    for(int i=0;i<workers;i++){
        PeerInfo peer; if(!mesh.getPeer(i,peer)) continue;
        jm.nonce_start=chunk*(uint32_t)(i+1);
        jm.nonce_end  =jm.nonce_start+chunk-1;
        jm.assigned_chunk=(uint8_t)(i+1);
        mesh.broadcastJob(jm);
        Serial.printf("[Redispatch] worker %d  nonce %08X-%08X\n",i+1,jm.nonce_start,jm.nonce_end);
    }

    // Shrink master's slice too
    MinerJob mj={};
    mj.valid=true; mj.job_id=currentJobId;
    strncpy((char*)mj.stratum_job_id,lastStratumJob.job_id,63);
    mj.nonce_start=0; mj.nonce_end=chunk-1;
    mj.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) mj.extranonce2[i]=(en2Counter>>(i*8))&0xFF;
    nbits_to_target(lastStratumJob.nbits,mj.target);
    memcpy(mj.header,    lastStratumJob.version,  4);
    memcpy(mj.header+4,  lastStratumJob.prev_hash,32);
    memcpy(mj.header+36, merkleRoot,              32);
    memcpy(mj.header+68, lastStratumJob.ntime,    4);
    memcpy(mj.header+72, lastStratumJob.nbits,    4);
    miner.setJob(mj);
    Serial.printf("[Redispatch] job=%d  workers=%d  chunk=%08X\n",currentJobId,workers,chunk);
}

// ───────────────────────────────────────────────────────────────
//  SECTION 14 — FALLBACK ELECTION
// ───────────────────────────────────────────────────────────────

static void checkElection(){
    if(isMaster||electionInProgress) return;
    uint32_t now=millis();
    if(masterLastSeenMs==0){masterLastSeenMs=now;return;}
    if(now-masterLastSeenMs>(uint32_t)(ELECTION_TRIGGER_COUNT*HEARTBEAT_INTERVAL_MS)){
        electionInProgress=true;
        Serial.println("[Election] master silent — electing");
        delay(random(0,ELECTION_BACKOFF_MAX_MS));
        MeshElectMsg e={};
        e.msg_type=MSG_ELECT; memcpy(e.candidate_mac,myMac,6);
        uint8_t pri=0; for(int i=0;i<6;i++) pri^=myMac[i];
        e.priority=pri;
        mesh.broadcastElection(e);
        delay(ELECTION_BACKOFF_MAX_MS);
        Serial.println("[Election] promoting to MASTER");
        isMaster=true;
        WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
        uint32_t t=millis();
        while(WiFi.status()!=WL_CONNECTED&&millis()-t<WIFI_TIMEOUT_MS) delay(200);
        if(WiFi.status()==WL_CONNECTED){
            stratum.onJob=[](const StratumJob& j){dispatchJob(j);};
            stratum.connect(POOL_HOST,POOL_PORT,POOL_USER,POOL_PASS);
            disp.showStatus("PROMOTED","Now master!");
        }
    }
}

// ───────────────────────────────────────────────────────────────
//  SECTION 15 — SETUP & LOOP
// ───────────────────────────────────────────────────────────────

// Actual WiFi channel — read after connect, ESP-NOW must match
static uint8_t g_wifiChannel = ESPNOW_CHANNEL;

static bool tryWifi(){
#if NODE_ROLE == ROLE_MASTER
    return true;
#elif NODE_ROLE == ROLE_WORKER
    return false;
#else
    Serial.printf("[Main] WiFi -> %s\n",WIFI_SSID);
    WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
    uint32_t t=millis();
    while(WiFi.status()!=WL_CONNECTED&&millis()-t<WIFI_TIMEOUT_MS){delay(200);Serial.print(".");}
    Serial.println();
    if(WiFi.status()==WL_CONNECTED){
        // Read the real channel the AP assigned — ESP-NOW MUST use this same channel
        wifi_second_chan_t second;
        esp_wifi_get_channel(&g_wifiChannel,&second);
        Serial.printf("[Main] IP: %s  WiFi channel: %d\n",
            WiFi.localIP().toString().c_str(),(int)g_wifiChannel);
        return true;
    }
    return false;
#endif
}

void setup(){
    Serial.begin(SERIAL_BAUD);
    delay(100);
    Serial.println("\n=== MeshMiner 32 ===");
    startupMs=millis();
    WiFi.macAddress(myMac);
    Serial.printf("[Main] MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
        myMac[0],myMac[1],myMac[2],myMac[3],myMac[4],myMac[5]);

    disp.begin();
    delay(1500);   // show splash

    isMaster=tryWifi();

    if(isMaster){
        // ── MASTER ────────────────────────────
        Serial.println("[Main] role=MASTER");
        disp.showStatus("MASTER","Mesh init...");
        mesh.begin(true, g_wifiChannel);

        stratum.onJob=[](const StratumJob& j){
            dispatchJob(j);
            // If workers were already seen before the first job arrived, send them work now
            if(mesh.peerCount()>0){
                Serial.printf("[Main] job arrived with %d workers waiting — redispatching\n",mesh.peerCount());
                redispatchToWorkers();
                lastRebroadcastMs=millis();
            }
        };

        mesh.onResult=[](const MeshResultMsg& r){
            Serial.printf("[Master] worker result nonce=%08X job=%d\n",r.nonce,r.job_id);
            if(lastStratumJob.valid){
                uint8_t en2[8]={};
                for(int i=0;i<4;i++) en2[i]=(en2Counter>>(i*8))&0xFF;
                stratum.submit(lastStratumJob,r.nonce,en2,stratum.session().extranonce2_len);
            }
            foundBlocks++;
        };

        mesh.onStats=[](const MeshStatsMsg&){
            totalWorkerHR=0;
            int cnt=mesh.peerCount();
            for(int i=0;i<cnt;i++){PeerInfo p;if(mesh.getPeer(i,p))totalWorkerHR+=p.hash_rate;}
        };

        miner.onFound=[](const MinerJob& job,uint32_t nonce){
            // DO NOT call WiFi/stratum here — we are on Core 1
            // Signal Core 0 via volatile flag to submit safely
            pendingNonce    = nonce;
            pendingNonceJob = job.job_id;
            pendingNonceReady = true;
            foundBlocks++;
        };

        disp.showStatus("MASTER","Connecting pool...");
        if(!stratum.connect(POOL_HOST,POOL_PORT,POOL_USER,POOL_PASS)){
            disp.showStatus("Pool FAILED","check POOL_USER");
            Serial.println("[Main] Pool connect failed — will retry in loop");
        } else {
            disp.showStatus("MASTER","Pool connected!");
        }

    } else {
        // ── WORKER ────────────────────────────
        Serial.println("[Main] role=WORKER");
        disp.showStatus("WORKER","Listening...");
        mesh.begin(false);

        mesh.onJob=[](const MeshJobMsg& j){
            Serial.printf("[Worker] job=%d  nonce %08X-%08X\n",j.job_id,j.nonce_start,j.nonce_end);
            MinerJob mj={};
            mj.valid=true; mj.job_id=j.job_id;
            mj.nonce_start=j.nonce_start; mj.nonce_end=j.nonce_end;
            mj.extranonce2_len=j.extranonce2_len;
            memcpy(mj.extranonce2,j.extranonce2,j.extranonce2_len);
            nbits_to_target(j.nbits,mj.target);
            memcpy(mj.header,    j.version,    4);
            memcpy(mj.header+4,  j.prev_hash,  32);
            memcpy(mj.header+36, j.merkle_root,32);
            memcpy(mj.header+68, j.ntime,      4);
            memcpy(mj.header+72, j.nbits,      4);
            miner.setJob(mj);
        };

        miner.onFound=[](const MinerJob& job,uint32_t nonce){
            // Signal Core 0 via flag — esp_now_send is not safe from Core 1
            pendingNonce    = nonce;
            pendingNonceJob = job.job_id;
            pendingNonceReady = true;
            foundBlocks++;
        };

        mesh.onHeartbeat=[](const MeshHeartbeatMsg& hb){
            if(hb.is_master){masterLastSeenMs=millis();electionInProgress=false;}
        };
    }

    miner.begin();

    // BOOT button (GPIO 0) for manual page flip — INPUT_PULLUP
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    // Blue LED — blinks at hash speed
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    lastPageFlipMs = millis();   // prevent instant flip on first loop tick
    lastActivityMs = millis();   // reset sleep timer at boot
    disp.setPage(PAGE_MINING);   // always start on the main mining page

    Serial.println("[Main] setup complete");
}

void loop(){
    uint32_t now=millis();

    // Cross-core nonce submission — handle found nonce from miner (Core 1) safely on Core 0
    if(pendingNonceReady){
        pendingNonceReady=false;
        uint32_t nonce = pendingNonce;
        Serial.printf("[Found] nonce=%08X job=%d\n", nonce, (int)pendingNonceJob);
        if(isMaster){
            if(lastStratumJob.valid){
                uint8_t en2[8]={};
                for(int i=0;i<4;i++) en2[i]=(en2Counter>>(i*8))&0xFF;
                stratum.submit(lastStratumJob,nonce,en2,stratum.session().extranonce2_len);
            }
        } else {
            uint8_t masterMac[6];
            if(mesh.getMasterMac(masterMac)){
                MeshResultMsg r={};
                r.msg_type=MSG_RESULT; r.job_id=pendingNonceJob; r.nonce=nonce;
                memcpy(r.worker_mac,myMac,6);
                mesh.sendResult(masterMac,r);
            }
        }
    }

    // Pool keep-alive + receive (master only)
    if(isMaster){
        if(!stratum.isConnected()){
            // Non-blocking retry — don't block the loop for 10s every tick
            if(now-lastPoolRetryMs>=POOL_RETRY_MS){
                lastPoolRetryMs=now;
                Serial.println("[Main] pool reconnecting...");
                stratum.connect(POOL_HOST,POOL_PORT,POOL_USER,POOL_PASS);
            }
        } else {
            stratum.loop();
        }
    }

    // Heartbeat broadcast
    if(now-lastHeartbeatMs>=HEARTBEAT_INTERVAL_MS){
        lastHeartbeatMs=now;
        MeshHeartbeatMsg hb={};
        hb.msg_type=MSG_HEARTBEAT;
        memcpy(hb.sender_mac,myMac,6);
        hb.is_master=isMaster?1:0;
        hb.uptime_s=(now-startupMs)/1000;
        hb.total_hash_rate=isMaster?totalWorkerHR+miner.getStats().hash_rate:miner.getStats().hash_rate;
        mesh.broadcastHeartbeat(hb);
        mesh.removeStalePeers(now);
    }

    // New worker joined — immediately re-slice and send jobs
    if(isMaster){
        // _newPeer flag is set inside the ESP-NOW ISR when addPeer() registers a new node
        if(mesh.takeNewPeer()){
            int curPeers=mesh.peerCount();   // read AFTER confirming new peer
            lastPeerCount=curPeers;
            Serial.printf("[Main] new peer -> %d workers\n",curPeers);
            if(lastStratumJob.valid){
                // Job already known — dispatch immediately
                Serial.println("[Main] job known, dispatching now");
                redispatchToWorkers();
                lastRebroadcastMs=now;
            } else {
                // Job not yet received — onJob callback will dispatch when it arrives
                Serial.println("[Main] waiting for first job from pool...");
            }
            // Always schedule a retry 1.5 s later as belt-and-braces
            _pendingRetry = true;
            _retryMs      = now + 1500;
        }
        // Retry dispatch ~800 ms after a new peer joined (belt-and-braces)
        if(_pendingRetry && now>=_retryMs){
            _pendingRetry=false;
            Serial.println("[Main] retry dispatch to new worker");
            redispatchToWorkers();
            lastRebroadcastMs=now;
        }
        // Periodic rebroadcast every 60s — catches workers that reset or missed a packet
        if(lastStratumJob.valid && mesh.peerCount()>0 && now-lastRebroadcastMs>=60000){
            lastRebroadcastMs=now;
            Serial.println("[Main] periodic rebroadcast");
            redispatchToWorkers();
        }
    }

    // Worker stats + election check
    if(!isMaster&&now-lastStatsMs>=5000){
        lastStatsMs=now;
        uint8_t masterMac[6];
        if(mesh.getMasterMac(masterMac)){
            MeshStatsMsg s={};
            s.msg_type=MSG_STATS;
            memcpy(s.worker_mac,myMac,6);
            s.hash_rate    =miner.getStats().hash_rate;
            s.nonces_tested=miner.getStats().total_hashes;
            s.job_id       =currentJobId;
            mesh.sendStats(masterMac,s);
        }
        checkElection();
    }

    // ── Display: page flip + refresh (unified block) ───────────
    {
        // Button check — BOOT button active LOW
        bool btn=digitalRead(BOOT_BTN_PIN);
        if(!btn && lastBtnState && (now-lastBtnMs)>200){
            lastBtnMs=now;
            lastActivityMs=now;          // reset sleep timer on any press
            if(disp.isSleeping()){
                disp.wake();             // first press just wakes — don't flip
                lastDisplayMs=0;         // force redraw immediately
                lastPageFlipMs=now;      // reset auto-flip timer
            } else {
                disp.nextPage();
                lastDisplayMs=0;
                lastPageFlipMs=now;
                Serial.println("[Display] btn flip");
            }
        }
        lastBtnState=btn;
    }

    // Display sleep timeout
    if(DISPLAY_SLEEP_MS > 0 && !disp.isSleeping() && (now-lastActivityMs)>=DISPLAY_SLEEP_MS){
        disp.sleep();
    }

    // Display refresh + auto page flip
    if(now-lastDisplayMs>=OLED_REFRESH_MS){
        lastDisplayMs=now;
        MinerStats ms=miner.getStats();
        DisplayData dd={};
        dd.hash_rate       =isMaster?totalWorkerHR+ms.hash_rate:ms.hash_rate;
        dd.total_hashes    =ms.total_hashes;
        dd.found_blocks    =foundBlocks;
        // Accumulate accepted shares — persists across reconnects
        {
            static uint32_t lastAcc=0;
            uint32_t curAcc=stratum.acceptedShares();
            if(curAcc>lastAcc){ acceptedShares+=curAcc-lastAcc; lastAcc=curAcc; }
        }
        dd.accepted_shares = acceptedShares;
        dd.is_master       =isMaster;
        dd.uptime_s        =(now-startupMs)/1000;
        dd.peer_count      =mesh.peerCount();
        strncpy(dd.pool_host,POOL_HOST,sizeof(dd.pool_host)-1);
        dd.pool_port       =POOL_PORT;
        dd.difficulty      =isMaster?stratum.session().difficulty:0;
        if(WiFi.status()==WL_CONNECTED){
            strncpy(dd.ssid,WiFi.SSID().c_str(),sizeof(dd.ssid)-1);
            strncpy(dd.ip,WiFi.localIP().toString().c_str(),sizeof(dd.ip)-1);
            dd.rssi=WiFi.RSSI();
        }
        int cnt=min(mesh.peerCount(),8);
        for(int i=0;i<cnt;i++){
            PeerInfo p;if(mesh.getPeer(i,p)){memcpy(dd.worker_macs[i],p.mac,6);dd.worker_hash_rates[i]=p.hash_rate;}
        }
        if(strlen(lastStratumJob.job_id)) strncpy(dd.job_id,lastStratumJob.job_id,15);
        dd.mining_active = (ms.total_hashes > 0);
        dd.tick          = (uint32_t)(now / 500);

        // Auto page flip — advance page every PAGE_FLIP_MS
        if(!disp.isSleeping() && now-lastPageFlipMs>=PAGE_FLIP_MS){
            lastPageFlipMs=now;
            disp.nextPage();
            Serial.printf("[Display] auto -> page %d\n", (int)((now/PAGE_FLIP_MS)%PAGE_COUNT));
        }

        disp.update(dd);
    }

    // Blue LED blinks at hash speed — faster = more hashes
    // At 30kH/s: blink every ~33ms. Cap at 50ms min so it's visible.
    {
        uint32_t hr = miner.getStats().hash_rate;
        uint32_t blinkInterval = (hr > 0) ? max(50UL, 1000000UL/hr) : 500;
        if(now-lastLedMs >= blinkInterval){
            lastLedMs=now;
            ledState=!ledState;
            digitalWrite(LED_PIN, ledState ? HIGH : LOW);
        }
    }

    delay(10);
}

 *  Libraries (install via Arduino Library Manager):
 *    ArduinoJson  >= 7.0   (bblanchon)
 *    U8g2         >= 2.35  (olikraus)
 *
 *  SPI OLED wiring (ESP32 DevKit V1):
 *  ┌─────────────┬───────────────────────────────┐
 *  │  OLED pin   │  ESP32 pin                    │
 *  ├─────────────┼───────────────────────────────┤
 *  │  VCC        │  3.3V                         │
 *  │  GND        │  GND                          │
 *  │  CLK / D0   │  GPIO 18  (HW SPI SCK)        │
 *  │  MOSI / D1  │  GPIO 23  (HW SPI MOSI)       │
 *  │  CS         │  GPIO  5                      │
 *  │  DC         │  GPIO 16                      │
 *  │  RST        │  GPIO 17                      │
 *  └─────────────┴───────────────────────────────┘
 *
 *  Monitor your miner at: https://web.public-pool.io
 *  Enter your BTC address to see hashrate & shares live.
 * ═══════════════════════════════════════════════════════════════
 */

// ───────────────────────────────────────────────────────────────
//  SECTION 1 — USER CONFIGURATION  (edit these before flashing)
// ───────────────────────────────────────────────────────────────

#define WIFI_SSID        "Home Assistant"
#define WIFI_PASSWORD    "7537819ajk"
#define WIFI_TIMEOUT_MS  20000

// public-pool.io — 0% fee solo pool, tracks workers by BTC address
// Append a worker name after a dot so it shows in the dashboard
// e.g.  bc1qXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX.esp32master
#define POOL_HOST   "public-pool.io"
#define POOL_PORT   3333
#define POOL_USER   "bc1qdh02k3mrznn038g62arfpe8n42nk9uc96hcpty.MeshMiner32"
#define POOL_PASS   "x"

#define STRATUM_TIMEOUT_MS  10000

// Node role:
//   ROLE_AUTO   = tries WiFi → if connected becomes master, else worker
//   ROLE_MASTER = always master (needs WiFi creds above)
//   ROLE_WORKER = always worker  (no WiFi needed)
#define ROLE_AUTO    0
#define ROLE_MASTER  1
#define ROLE_WORKER  2
#define NODE_ROLE    ROLE_AUTO

// ── SH1106 SPI pins ──────────────────────────────────────────
//  SCL (CLK)  -> GPIO 18  (ESP32 HW SPI SCK,  label D18)
//  SDA (MOSI) -> GPIO 23  (ESP32 HW SPI MOSI, label D23)
//  DC         -> GPIO  2  (label D2)
//  RES        -> GPIO  4  (label D4)
//  CS         -> GND on module — use U8X8_PIN_NONE in code
#define OLED_DC   2
#define OLED_RST  4

#define OLED_REFRESH_MS   1000
#define PAGE_FLIP_MS      3000   // auto-advance page every N ms
#define BOOT_BTN_PIN        0    // GPIO0 = BOOT button on DevKit V1

// ── Mining task ──────────────────────────────────────────────
#define MINING_TASK_PRIORITY  5
#define MINING_TASK_CORE      1      // core 1 — WiFi stays on core 0
#define MINING_TASK_STACK     12288

// ── ESP-NOW mesh ─────────────────────────────────────────────
#define ESPNOW_CHANNEL          1
#define PEER_TIMEOUT_MS         15000
#define HEARTBEAT_INTERVAL_MS   5000
#define ELECTION_TRIGGER_COUNT  3
#define ELECTION_BACKOFF_MAX_MS 2000
#define MESH_MAX_PEERS          20
#define MESH_FRAME_LEN          250

#define SERIAL_BAUD       115200
#define LED_PIN           2    // Built-in blue LED on most ESP32 DevKit V1 boards
#define POOL_RETRY_MS  15000   // Only retry pool connection every 15s (non-blocking)

// ───────────────────────────────────────────────────────────────
//  SECTION 2 — INCLUDES
// ───────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>
#include <functional>
#include <string.h>

// ───────────────────────────────────────────────────────────────
//  SECTION 3 — FORWARD DECLARATIONS
//  (prevents Arduino IDE preprocessor ordering issues)
// ───────────────────────────────────────────────────────────────

struct StratumJob;
struct StratumSession;
struct MinerJob;
struct MinerStats;
struct PeerInfo;
struct DisplayData;

static void dispatchJob(const StratumJob& sj);
static void checkElection();

// ───────────────────────────────────────────────────────────────
//  SECTION 4 — SHA-256  (double SHA for Bitcoin mining)
// ───────────────────────────────────────────────────────────────

static const uint32_t SHA_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static const uint32_t SHA_H0[8] = {
    0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
};

#define ROTR32(x,n)    (((x)>>(n))|((x)<<(32-(n))))
#define SHA_CH(e,f,g)  (((e)&(f))^(~(e)&(g)))
#define SHA_MAJ(a,b,c) (((a)&(b))^((a)&(c))^((b)&(c)))
#define SHA_EP0(a)     (ROTR32(a,2) ^ROTR32(a,13)^ROTR32(a,22))
#define SHA_EP1(e)     (ROTR32(e,6) ^ROTR32(e,11)^ROTR32(e,25))
#define SHA_SIG0(x)    (ROTR32(x,7) ^ROTR32(x,18)^((x)>>3))
#define SHA_SIG1(x)    (ROTR32(x,17)^ROTR32(x,19)^((x)>>10))

static inline uint32_t sha_be32(const uint8_t* p){
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static inline void sha_wr32(uint8_t* p,uint32_t v){
    p[0]=(v>>24)&0xFF;p[1]=(v>>16)&0xFF;p[2]=(v>>8)&0xFF;p[3]=v&0xFF;
}

static void sha256_compress(const uint32_t* in16,uint32_t* st){
    uint32_t w[64];
    for(int i=0;i<16;i++) w[i]=in16[i];
    for(int i=16;i<64;i++) w[i]=SHA_SIG1(w[i-2])+w[i-7]+SHA_SIG0(w[i-15])+w[i-16];
    uint32_t a=st[0],b=st[1],c=st[2],d=st[3],e=st[4],f=st[5],g=st[6],h=st[7];
    for(int i=0;i<64;i++){
        uint32_t t1=h+SHA_EP1(e)+SHA_CH(e,f,g)+SHA_K[i]+w[i];
        uint32_t t2=SHA_EP0(a)+SHA_MAJ(a,b,c);
        h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
    }
    st[0]+=a;st[1]+=b;st[2]+=c;st[3]+=d;
    st[4]+=e;st[5]+=f;st[6]+=g;st[7]+=h;
}

static void double_sha256(const uint8_t* data,size_t len,uint8_t* digest){
    uint8_t mid[32];
    {
        uint32_t st[8]; memcpy(st,SHA_H0,32);
        uint8_t buf[64];
        size_t rem=len; const uint8_t* ptr=data;
        while(rem>=64){
            uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(ptr+i*4);
            sha256_compress(w,st); ptr+=64; rem-=64;
        }
        memset(buf,0,64); memcpy(buf,ptr,rem); buf[rem]=0x80;
        if(rem>=56){
            uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(buf+i*4);
            sha256_compress(w,st); memset(buf,0,64);
        }
        uint64_t bits=(uint64_t)len*8;
        for(int i=0;i<8;i++) buf[56+i]=(bits>>((7-i)*8))&0xFF;
        uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(buf+i*4);
        sha256_compress(w,st);
        for(int i=0;i<8;i++) sha_wr32(mid+i*4,st[i]);
    }
    {
        uint32_t st[8]; memcpy(st,SHA_H0,32);
        uint8_t buf[64]={0}; memcpy(buf,mid,32); buf[32]=0x80;
        buf[62]=0x01; buf[63]=0x00;
        uint32_t w[16]; for(int i=0;i<16;i++) w[i]=sha_be32(buf+i*4);
        sha256_compress(w,st);
        for(int i=0;i<8;i++) sha_wr32(digest+i*4,st[i]);
    }
}

static void bitcoin_hash(const uint8_t* h80,uint8_t* o32){ double_sha256(h80,80,o32); }

static bool check_difficulty(const uint8_t* hash,const uint8_t* tgt){
    for(int i=31;i>=0;i--){
        if(hash[i]<tgt[i]) return true;
        if(hash[i]>tgt[i]) return false;
    }
    return true;
}

static void nbits_to_target(const uint8_t* nb,uint8_t* t32){
    memset(t32,0,32); uint8_t exp=nb[0]; if(exp==0||exp>32) return;
    int pos=(int)exp-1;
    if(pos>=0   &&pos<32) t32[pos]  =nb[1];
    if(pos-1>=0 &&pos-1<32) t32[pos-1]=nb[2];
    if(pos-2>=0 &&pos-2<32) t32[pos-2]=nb[3];
}

// ───────────────────────────────────────────────────────────────
//  SECTION 5 — HEX HELPERS
// ───────────────────────────────────────────────────────────────

static void hexToBytes(const char* hex,uint8_t* out,int maxLen){
    int len=(int)(strlen(hex)/2); if(len>maxLen) len=maxLen;
    for(int i=0;i<len;i++){
        auto nib=[](char c)->uint8_t{
            if(c>='0'&&c<='9') return c-'0';
            if(c>='a'&&c<='f') return c-'a'+10;
            if(c>='A'&&c<='F') return c-'A'+10;
            return 0;
        };
        out[i]=(nib(hex[i*2])<<4)|nib(hex[i*2+1]);
    }
}

static String bytesToHex(const uint8_t* b,int len){
    static const char* H="0123456789abcdef";
    String s; s.reserve(len*2);
    for(int i=0;i<len;i++){s+=H[(b[i]>>4)&0xF];s+=H[b[i]&0xF];}
    return s;
}

// ───────────────────────────────────────────────────────────────
//  SECTION 6 — STRATUM STRUCTS
// ───────────────────────────────────────────────────────────────

struct StratumJob {
    char     job_id[64];
    uint8_t  prev_hash[32];
    uint8_t  coinbase1[128]; uint16_t coinbase1_len;
    uint8_t  coinbase2[128]; uint16_t coinbase2_len;
    uint8_t  merkle_branches[16][32];
    uint8_t  merkle_count;
    uint8_t  version[4];
    uint8_t  nbits[4];
    uint8_t  ntime[4];
    bool     clean_jobs;
    bool     valid;
};

struct StratumSession {
    uint8_t  extranonce1[8];
    uint8_t  extranonce1_len;
    uint8_t  extranonce2_len;
    uint32_t difficulty;
    bool     subscribed;
    bool     authorized;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 7 — ESP-NOW MESH STRUCTS
// ───────────────────────────────────────────────────────────────

#define MSG_JOB        0x01
#define MSG_RESULT     0x02
#define MSG_STATS      0x03
#define MSG_HEARTBEAT  0x04
#define MSG_ELECT      0x05

static const uint8_t ESPNOW_BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

#pragma pack(push,1)
typedef struct {
    uint8_t  msg_type;
    uint8_t  job_id;
    uint8_t  version[4];
    uint8_t  prev_hash[32];
    uint8_t  merkle_root[32];
    uint8_t  nbits[4];
    uint8_t  ntime[4];
    uint32_t nonce_start;
    uint32_t nonce_end;
    uint8_t  extranonce2[8];
    uint8_t  extranonce2_len;
    uint8_t  assigned_chunk;
} MeshJobMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  job_id;
    uint32_t nonce;
    uint8_t  worker_mac[6];
} MeshResultMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  worker_mac[6];
    uint32_t hash_rate;
    uint32_t nonces_tested;
    uint8_t  job_id;
} MeshStatsMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  sender_mac[6];
    uint8_t  is_master;
    uint32_t uptime_s;
    uint32_t total_hash_rate;
} MeshHeartbeatMsg;

typedef struct {
    uint8_t  msg_type;
    uint8_t  candidate_mac[6];
    uint8_t  priority;
} MeshElectMsg;
#pragma pack(pop)

struct PeerInfo {
    uint8_t  mac[6];
    bool     active;
    uint32_t last_seen_ms;
    uint32_t hash_rate;
    uint8_t  assigned_chunk;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 8 — EspNowMesh CLASS
// ───────────────────────────────────────────────────────────────

class EspNowMesh {
public:
    static EspNowMesh& instance(){ static EspNowMesh i; return i; }

    bool begin(bool isMaster, uint8_t channel=ESPNOW_CHANNEL){
        _isMaster=isMaster;
        _channel=channel;
        if(isMaster) WiFi.mode(WIFI_AP_STA);
        else       { WiFi.mode(WIFI_STA); WiFi.disconnect(); }
        // Force ESP-NOW onto the same channel WiFi is using
        esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
        Serial.printf("[Mesh] ESP-NOW channel: %d\n",(int)_channel);
        if(esp_now_init()!=ESP_OK){ Serial.println("[Mesh] init failed"); return false; }
        esp_now_register_recv_cb(_recv_cb);
        esp_now_register_send_cb(_send_cb);
        esp_now_peer_info_t bc={};
        memcpy(bc.peer_addr,ESPNOW_BROADCAST,6);
        bc.channel=_channel; bc.encrypt=false;
        if(esp_now_add_peer(&bc)!=ESP_OK){ Serial.println("[Mesh] bcast fail"); return false; }
        _initialized=true;
        Serial.printf("[Mesh] ready as %s\n",isMaster?"MASTER":"WORKER");
        return true;
    }

    bool broadcastJob(const MeshJobMsg& j)                  { return _tx(ESPNOW_BROADCAST,&j,sizeof(j)); }
    bool sendResult(const uint8_t* m,const MeshResultMsg& r){ return _tx(m,&r,sizeof(r)); }
    bool sendStats(const uint8_t* m,const MeshStatsMsg& s)  { return _tx(m,&s,sizeof(s)); }
    bool broadcastHeartbeat(const MeshHeartbeatMsg& h)       { return _tx(ESPNOW_BROADCAST,&h,sizeof(h)); }
    bool broadcastElection(const MeshElectMsg& e)            { return _tx(ESPNOW_BROADCAST,&e,sizeof(e)); }

    bool addPeer(const uint8_t* mac){
        bool bc=true; for(int i=0;i<6;i++) if(mac[i]!=0xFF){bc=false;break;} if(bc) return true;
        for(int i=0;i<_peerCount;i++) if(memcmp(_peers[i].mac,mac,6)==0) return true;
        if(_peerCount>=MESH_MAX_PEERS) return false;
        if(!esp_now_is_peer_exist(mac)){
            esp_now_peer_info_t pi={}; memcpy(pi.peer_addr,mac,6);
            pi.channel=_channel; pi.encrypt=false;
            if(esp_now_add_peer(&pi)!=ESP_OK) return false;
        }
        PeerInfo& p=_peers[_peerCount++];
        memcpy(p.mac,mac,6); p.active=true; p.last_seen_ms=millis(); p.hash_rate=0; p.assigned_chunk=0;
        Serial.printf("[Mesh] +peer %02X:%02X:%02X:%02X:%02X:%02X\n",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        _newPeer=true;   // signal main loop to redispatch
        return true;
    }

    void removeStalePeers(uint32_t now){
        for(int i=0;i<_peerCount;i++)
            if(_peers[i].active&&(now-_peers[i].last_seen_ms)>PEER_TIMEOUT_MS)
                _peers[i].active=false;
    }

    int  peerCount() const { int c=0; for(int i=0;i<_peerCount;i++) if(_peers[i].active) c++; return c; }

    bool getPeer(int idx,PeerInfo& out) const {
        int a=0;
        for(int i=0;i<_peerCount;i++) if(_peers[i].active){ if(a==idx){out=_peers[i];return true;} a++; }
        return false;
    }

    void updatePeerStats(const uint8_t* mac,uint32_t hr,uint32_t now){
        for(int i=0;i<_peerCount;i++) if(memcmp(_peers[i].mac,mac,6)==0){
            _peers[i].hash_rate=hr; _peers[i].last_seen_ms=now; _peers[i].active=true; return;
        }
        addPeer(mac); updatePeerStats(mac,hr,now);
    }

    void setMasterMac(const uint8_t* mac){ memcpy(_masterMac,mac,6); _hasMaster=true; addPeer(mac); }
    bool getMasterMac(uint8_t* out) const { if(!_hasMaster) return false; memcpy(out,_masterMac,6); return true; }
    bool hasMaster() const { return _hasMaster; }

    std::function<void(const MeshJobMsg&)>        onJob;
    std::function<void(const MeshResultMsg&)>     onResult;
    std::function<void(const MeshStatsMsg&)>      onStats;
    std::function<void(const MeshHeartbeatMsg&)>  onHeartbeat;
    std::function<void(const MeshElectMsg&)>      onElect;

    // Check and clear the new-peer flag (set by ISR when a worker is first seen)
    bool takeNewPeer(){ if(_newPeer){_newPeer=false;return true;} return false; }

private:
    EspNowMesh()=default;

    bool _tx(const uint8_t* mac,const void* data,size_t len){
        if(!_initialized||len>MESH_FRAME_LEN) return false;
        uint8_t frame[MESH_FRAME_LEN]={0}; memcpy(frame,data,len);
        return esp_now_send(mac,frame,MESH_FRAME_LEN)==ESP_OK;
    }

    // ESP32 core 3.x callback signatures
    static void _recv_cb(const esp_now_recv_info_t* info,const uint8_t* data,int len){
        if(len<1||!info) return;
        const uint8_t* mac=info->src_addr;
        EspNowMesh& m=instance(); m.addPeer(mac);
        uint8_t type=data[0];
        switch(type){
            case MSG_JOB:       if(len>=(int)sizeof(MeshJobMsg))      {MeshJobMsg       x;memcpy(&x,data,sizeof(x));if(m.onJob)      m.onJob(x);}      break;
            case MSG_RESULT:    if(len>=(int)sizeof(MeshResultMsg))   {MeshResultMsg    x;memcpy(&x,data,sizeof(x));if(m.onResult)   m.onResult(x);}   break;
            case MSG_STATS:     if(len>=(int)sizeof(MeshStatsMsg))    {MeshStatsMsg     x;memcpy(&x,data,sizeof(x));m.updatePeerStats(mac,x.hash_rate,millis());if(m.onStats)m.onStats(x);} break;
            case MSG_HEARTBEAT: if(len>=(int)sizeof(MeshHeartbeatMsg)){MeshHeartbeatMsg x;memcpy(&x,data,sizeof(x));
                                    if(x.is_master&&!m._hasMaster){m.setMasterMac(mac);Serial.printf("[Mesh] master %02X:%02X:%02X\n",mac[0],mac[1],mac[2]);}
                                    m.updatePeerStats(mac,x.total_hash_rate,millis());if(m.onHeartbeat)m.onHeartbeat(x);} break;
            case MSG_ELECT:     if(len>=(int)sizeof(MeshElectMsg))    {MeshElectMsg     x;memcpy(&x,data,sizeof(x));if(m.onElect)    m.onElect(x);}    break;
            default: break;
        }
    }
    static void _send_cb(const wifi_tx_info_t*,esp_now_send_status_t){}

    PeerInfo _peers[MESH_MAX_PEERS];
    int      _peerCount   = 0;
    bool     _initialized = false;
    bool     _isMaster    = false;
    bool     _hasMaster   = false;
    uint8_t  _masterMac[6]= {0};
    uint8_t  _channel     = ESPNOW_CHANNEL;
    volatile bool _newPeer = false;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 9 — STRATUM CLIENT
// ───────────────────────────────────────────────────────────────

class StratumClient {
public:
    bool connect(const char* host,uint16_t port,const char* user,const char* pass){
        Serial.printf("[Stratum] -> %s:%d\n",host,port);
        if(!_client.connect(host,port)){Serial.println("[Stratum] connection failed");return false;}
        _lastAct=millis();
        _tx("{\"id\":"+String(_id++)+",\"method\":\"mining.subscribe\",\"params\":[\"MeshMiner32/1.0\",null]}\n");
        uint32_t t=millis();
        while(millis()-t<STRATUM_TIMEOUT_MS){
            String line; if(_rxLine(line,500)){
                JsonDocument d;
                if(deserializeJson(d,line)==DeserializationError::Ok) _parseSub(d);
                if(_s.subscribed) break;
            }
            delay(10);
        }
        if(!_s.subscribed){Serial.println("[Stratum] subscribe timeout");return false;}
        _tx("{\"id\":"+String(_id++)+",\"method\":\"mining.authorize\",\"params\":[\""+String(user)+"\",\""+String(pass)+"\"]}\n");
        Serial.println("[Stratum] authorized, waiting for job...");
        return true;
    }

    void disconnect(){ _client.stop(); }
    bool isConnected(){ return _client.connected(); }

    void loop(){
        if(!_client.connected()) return;
        if(millis()-_lastAct>30000){
            _tx("{\"id\":"+String(_id++)+",\"method\":\"mining.ping\",\"params\":[]}\n");
            _lastAct=millis();
        }
        while(_client.available()){
            char c=_client.read();
            if(c=='\n'){if(_buf.length()>0)_procLine(_buf);_buf="";}
            else _buf+=c;
            _lastAct=millis();
        }
    }

    bool submit(const StratumJob& job,uint32_t nonce,const uint8_t* en2,uint8_t en2len){
        String msg="{\"id\":"+String(_id++)+",\"method\":\"mining.submit\",\"params\":[\""
            +String(POOL_USER)+"\",\""+String(job.job_id)+"\",\""
            +bytesToHex(en2,en2len)+"\",\""+bytesToHex(job.ntime,4)+"\",\""
            +bytesToHex((const uint8_t*)&nonce,4)+"\"]}\n";
        _submits++; return _tx(msg);
    }

    void computeMerkleRoot(uint8_t* root,const uint8_t* coinbase,uint16_t cbLen){
        double_sha256(coinbase,cbLen,root);
        for(int i=0;i<_job.merkle_count;i++){
            uint8_t pair[64]; memcpy(pair,root,32); memcpy(pair+32,_job.merkle_branches[i],32);
            double_sha256(pair,64,root);
        }
    }

    std::function<void(const StratumJob&)> onJob;

    const StratumJob&     currentJob()     const { return _job; }
    const StratumSession& session()        const { return _s; }
    uint32_t              acceptedShares() const { return _accepted; }
    uint32_t              totalSubmits()   const { return _submits; }

private:
    bool _tx(const String& msg){ if(!_client.connected()) return false; _client.print(msg); return true; }

    bool _rxLine(String& out,uint32_t tms){
        uint32_t t=millis();
        while(millis()-t<tms){if(_client.available()){out=_client.readStringUntil('\n');return out.length()>0;}delay(5);}
        return false;
    }

    void _procLine(const String& line){
        JsonDocument doc;
        if(deserializeJson(doc,line)!=DeserializationError::Ok) return;
        const char* method=doc["method"];
        if(method){
            if(strcmp(method,"mining.notify")==0)              _parseNotify(doc);
            else if(strcmp(method,"mining.set_difficulty")==0) _parseDiff(doc);
        } else {
            if((doc["result"]|false)&&(doc["id"]|0)>0) _accepted++;
        }
    }

    void _parseNotify(const JsonDocument& doc){
        JsonArrayConst p=doc["params"];
        if(p.isNull()||p.size()<9) return;
        StratumJob j={};
        strncpy(j.job_id,p[0]|"",sizeof(j.job_id)-1);
        hexToBytes(p[1]|"",j.prev_hash,32);
        j.coinbase1_len=strlen(p[2]|"")/2; hexToBytes(p[2]|"",j.coinbase1,sizeof(j.coinbase1));
        j.coinbase2_len=strlen(p[3]|"")/2; hexToBytes(p[3]|"",j.coinbase2,sizeof(j.coinbase2));
        JsonArrayConst br=p[4];
        j.merkle_count=min((int)br.size(),16);
        for(int i=0;i<j.merkle_count;i++) hexToBytes(br[i]|"",j.merkle_branches[i],32);
        hexToBytes(p[5]|"",j.version,4);
        hexToBytes(p[6]|"",j.nbits,4);
        hexToBytes(p[7]|"",j.ntime,4);
        j.clean_jobs=p[8]|false; j.valid=true;
        _job=j;
        Serial.printf("[Stratum] job %s  clean=%d\n",j.job_id,(int)j.clean_jobs);
        if(onJob) onJob(_job);
    }

    void _parseDiff(const JsonDocument& doc){
        JsonArrayConst p=doc["params"]; if(p.isNull()) return;
        _s.difficulty=p[0]|1;
        Serial.printf("[Stratum] difficulty %u\n",_s.difficulty);
    }

    void _parseSub(const JsonDocument& doc){
        JsonVariantConst r=doc["result"]; if(r.isNull()) return;
        const char* en1=r[1]|"";
        _s.extranonce1_len=min((int)(strlen(en1)/2),8);
        hexToBytes(en1,_s.extranonce1,_s.extranonce1_len);
        _s.extranonce2_len=r[2]|4;
        _s.subscribed=true;
        Serial.printf("[Stratum] subscribed  en1=%d  en2_size=%d\n",_s.extranonce1_len,_s.extranonce2_len);
    }

    WiFiClient     _client;
    StratumJob     _job    = {};
    StratumSession _s      = {};
    int            _id     = 1;
    uint32_t       _submits  = 0;
    uint32_t       _accepted = 0;
    uint32_t       _lastAct  = 0;
    String         _buf;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 10 — MINER  (FreeRTOS task, Core 1)
// ───────────────────────────────────────────────────────────────

struct MinerJob {
    uint8_t  header[80];
    uint8_t  target[32];
    uint32_t nonce_start;
    uint32_t nonce_end;
    uint8_t  job_id;
    uint8_t  stratum_job_id[64];
    uint8_t  extranonce2[8];
    uint8_t  extranonce2_len;
    bool     valid;
};

struct MinerStats {
    uint32_t hash_rate;
    uint32_t total_hashes;
    uint32_t found_nonces;
    uint32_t last_update_ms;
};

class Miner {
public:
    static Miner& instance(){ static Miner m; return m; }

    void begin(){
        _mutex=xSemaphoreCreateMutex();
        xTaskCreatePinnedToCore(_task,"mining",MINING_TASK_STACK,this,MINING_TASK_PRIORITY,nullptr,MINING_TASK_CORE);
        Serial.printf("[Miner] task on core %d\n",MINING_TASK_CORE);
    }

    void setJob(const MinerJob& j){
        if(!_mutex) return;
        xSemaphoreTake(_mutex,portMAX_DELAY); _pending=j; _newJob=true; xSemaphoreGive(_mutex);
    }

    MinerStats getStats() const { return _stats; }

    std::function<void(const MinerJob&,uint32_t)> onFound;

private:
    Miner()=default;
    static void _task(void* p){ static_cast<Miner*>(p)->_run(); vTaskDelete(nullptr); }

    void _run(){
        uint8_t hash[32];
        uint32_t cnt=0,wstart=millis();
        while(true){
            if(_newJob&&_mutex&&xSemaphoreTake(_mutex,0)==pdTRUE){
                _job=_pending; _newJob=false; cnt=0; wstart=millis(); xSemaphoreGive(_mutex);
                Serial.printf("[Miner] job=%d  %08X-%08X\n",_job.job_id,_job.nonce_start,_job.nonce_end);
            }
            if(!_job.valid){ vTaskDelay(pdMS_TO_TICKS(50)); continue; }
            for(uint32_t n=_job.nonce_start;n<_job.nonce_end&&!_newJob;n++){
                _job.header[76]= n     &0xFF;
                _job.header[77]=(n>> 8)&0xFF;
                _job.header[78]=(n>>16)&0xFF;
                _job.header[79]=(n>>24)&0xFF;
                bitcoin_hash(_job.header,hash);
                cnt++;
                if(check_difficulty(hash,_job.target)){
                    _stats.found_nonces++;
                    Serial.printf("[Miner] *** FOUND nonce=%08X ***\n",n);
                    if(onFound) onFound(_job,n);
                }
                if((cnt&0x3FF)==0){
                    uint32_t el=millis()-wstart;
                    if(el>0) _stats.hash_rate=(cnt*1000UL)/el;
                    _stats.total_hashes+=1024;
                    _stats.last_update_ms=millis();
                    // vTaskDelay(1) feeds the watchdog AND yields properly
                    vTaskDelay(1);
                }
            }
            if(!_newJob) _job.valid=false;
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    volatile bool     _newJob  = false;
    MinerJob          _job     = {};
    MinerJob          _pending = {};
    MinerStats        _stats   = {};
    SemaphoreHandle_t _mutex   = nullptr;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 11 — SH1106 SPI OLED DISPLAY  (U8g2 HW SPI)
// ───────────────────────────────────────────────────────────────

enum DisplayPage { PAGE_MINING=0,PAGE_POOL,PAGE_NETWORK,PAGE_MESH,PAGE_COUNT };

struct DisplayData {
    uint32_t hash_rate,total_hashes,found_blocks,accepted_shares;
    char     job_id[16];
    char     pool_host[32]; uint16_t pool_port; uint32_t difficulty;
    char     ssid[32]; int8_t rssi; char ip[16];
    int      peer_count;
    uint32_t worker_hash_rates[8];
    uint8_t  worker_macs[8][6];
    bool     is_master;
    uint32_t uptime_s;
    bool     mining_active;  // true once miner has processed at least one batch
    uint32_t tick;           // increments each display refresh — drives animations
};

class OledDisplay {
public:
    static OledDisplay& instance(){ static OledDisplay o; return o; }

    void begin(){
        _u8g2.begin();
        _u8g2.setContrast(200);
        _on=true;
        _splash();
    }

    void showStatus(const char* l1,const char* l2=nullptr){
        if(!_on) return;
        _u8g2.clearBuffer();
        _u8g2.setFont(u8g2_font_6x10_tf);
        _u8g2.drawStr(0,14,l1);
        if(l2) _u8g2.drawStr(0,28,l2);
        _u8g2.sendBuffer();
    }

    void nextPage(){ _page=(DisplayPage)(((int)_page+1)%PAGE_COUNT); }
    void setPage(DisplayPage p){ _page=p; }

    void update(const DisplayData& d){
        if(!_on) return;
        _u8g2.clearBuffer();
        switch(_page){
            case PAGE_MINING:  _drawMining(d);  break;
            case PAGE_POOL:    _drawPool(d);    break;
            case PAGE_NETWORK: _drawNet(d);     break;
            case PAGE_MESH:    _drawMesh(d);    break;
            default: break;
        }
        // page indicator dots — bottom right
        for(int i=0;i<PAGE_COUNT;i++){
            if(i==(int)_page) _u8g2.drawBox(122-(PAGE_COUNT-1-i)*6,61,4,3);
            else              _u8g2.drawFrame(122-(PAGE_COUNT-1-i)*6,61,4,3);
        }
        _u8g2.sendBuffer();
    }

private:
    OledDisplay()=default;

    // SPI OLED with no CS pin (CS tied to GND on module).
    // U8G2_SH1106 4W_HW_SPI: (rotation, cs, dc, reset)
    // Pass U8X8_PIN_NONE for cs — library skips the CS toggle.
    U8G2_SH1106_128X64_NONAME_F_4W_HW_SPI _u8g2{
        U8G2_R0,
        /* cs=   */ U8X8_PIN_NONE,
        /* dc=   */ OLED_DC,
        /* reset=*/ OLED_RST
    };

    DisplayPage _page = PAGE_MINING;
    bool        _on   = false;

    void _splash(){
        _u8g2.clearBuffer();
        // ── Bitcoin ₿ symbol drawn with primitives ──
        // Outer circle
        _u8g2.drawCircle(32,26,18);
        // Vertical stem top
        _u8g2.drawVLine(32,7,4);
        // Vertical stem bottom
        _u8g2.drawVLine(32,41,4);
        // Left vertical bar of B
        _u8g2.drawVLine(27,14,24);
        _u8g2.drawVLine(28,14,24);
        // Top bump of B
        _u8g2.drawHLine(28,14,8);
        _u8g2.drawHLine(28,23,8);
        _u8g2.drawVLine(36,14,9);
        // Bottom bump of B (wider)
        _u8g2.drawHLine(28,24,9);
        _u8g2.drawHLine(28,38,9);
        _u8g2.drawVLine(37,24,14);
        // Slight tilt lines for $ style
        _u8g2.drawVLine(31,7,4);
        _u8g2.drawVLine(31,41,4);
        // ── Name — right half of screen (x=56 to 128 = 72px wide) ──
        // u8g2_font_9x15_tf: each char ~9px wide
        // "MeshMiner" = 9 chars = ~63px  fits in 72px
        // "32"        = 2 chars = ~18px
        _u8g2.setFont(u8g2_font_9x15_tf);
        _u8g2.drawStr(57,26,"Mesh");
        _u8g2.drawStr(57,44,"Miner 32");
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(57,57,"public-pool.io");
        _u8g2.sendBuffer();
    }

    static String _fmtHR(uint32_t h){
        if(h>=1000000) return String(h/1000000.0f,1)+"MH/s";
        if(h>=1000)    return String(h/1000.0f,1)+"kH/s";
        return String(h)+"H/s";
    }
    static String _fmtUp(uint32_t s){
        if(s<60)   return String(s)+"s";
        if(s<3600) return String(s/60)+"m";
        return String(s/3600)+"h";
    }

    void _drawMining(const DisplayData& d){
        // ── Header: name + spinner + uptime — all one line ──
        _u8g2.setFont(u8g2_font_5x7_tf);
        // Spinner indicator (| / - \) when mining, * when idle
        const char* spin[]={"| ","/ ","- ","\\ "};
        const char* ind = d.mining_active ? spin[d.tick%4] : "* ";
        char hdr[28];
        snprintf(hdr,sizeof(hdr),"%sMeshMiner32",ind);
        _u8g2.drawStr(0,7,hdr);
        char upbuf[12]; snprintf(upbuf,12,"UP:%s",_fmtUp(d.uptime_s).c_str());
        _u8g2.drawStr(90,7,upbuf);
        _u8g2.drawHLine(0,9,128);

        // ── Hashrate (big font) ─────────────────────
        _u8g2.setFont(u8g2_font_logisoso16_tr);
        String hr=d.mining_active?_fmtHR(d.hash_rate):"--";
        _u8g2.drawStr(0,30,hr.c_str());

        // ── Stats rows ──────────────────────────────
        _u8g2.setFont(u8g2_font_5x7_tf);
        char p1[18]; snprintf(p1,18,"Peers:%-2d",d.peer_count);
        _u8g2.drawStr(0,44,p1);
        char p2[18]; snprintf(p2,18,"Acc:%-5lu",(unsigned long)d.accepted_shares);
        _u8g2.drawStr(66,44,p2);
        char p3[18]; snprintf(p3,18,"Blk:%-3lu",(unsigned long)d.found_blocks);
        _u8g2.drawStr(0,55,p3);
        if(strlen(d.job_id)){
            char jbuf[12]; snprintf(jbuf,12,"J:%.6s",d.job_id);
            _u8g2.drawStr(66,55,jbuf);
        }
    }

    void _drawPool(const DisplayData& d){
        // Inverted header bar
        _u8g2.drawBox(0,0,128,10);
        _u8g2.setColorIndex(0);
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(2,8,"POOL");
        _u8g2.setColorIndex(1);
        _u8g2.setFont(u8g2_font_6x10_tf);
        char h[22]; snprintf(h,sizeof(h),"%.21s",d.pool_host);
        _u8g2.drawStr(0,22,h);
        _u8g2.drawStr(0,34,("Port: "+String(d.pool_port)).c_str());
        _u8g2.drawStr(0,46,("Diff: "+String(d.difficulty)).c_str());
        _u8g2.drawStr(0,58,("Acc: "+String(d.accepted_shares)).c_str());
    }

    void _drawNet(const DisplayData& d){
        _u8g2.drawBox(0,0,128,10);
        _u8g2.setColorIndex(0);
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(2,8,"NETWORK");
        _u8g2.setColorIndex(1);
        _u8g2.setFont(u8g2_font_6x10_tf);
        char s[22]; snprintf(s,sizeof(s),"%.21s",d.ssid); _u8g2.drawStr(0,22,s);
        _u8g2.drawStr(0,34,d.ip);
        _u8g2.drawStr(0,46,("RSSI: "+String(d.rssi)+"dBm").c_str());
        _u8g2.drawStr(0,58,("Mesh: "+String(d.peer_count)+" nodes").c_str());
    }

    void _drawMesh(const DisplayData& d){
        _u8g2.drawBox(0,0,128,10);
        _u8g2.setColorIndex(0);
        _u8g2.setFont(u8g2_font_5x7_tf);
        _u8g2.drawStr(2,8,"MESH WORKERS");
        _u8g2.setColorIndex(1);
        int cnt=min(d.peer_count,6);
        for(int i=0;i<cnt;i++){
            char line[28];
            snprintf(line,sizeof(line),"%02X:%02X:%02X %s",
                d.worker_macs[i][3],d.worker_macs[i][4],d.worker_macs[i][5],
                _fmtHR(d.worker_hash_rates[i]).c_str());
            _u8g2.drawStr(0,20+i*9,line);
        }
        if(d.peer_count==0){
            _u8g2.setFont(u8g2_font_6x10_tf);
            _u8g2.drawStr(10,38,"No workers");
            _u8g2.drawStr(10,52,"yet");
        }
    }
};

// ───────────────────────────────────────────────────────────────
//  SECTION 12 — GLOBAL STATE
// ───────────────────────────────────────────────────────────────

static EspNowMesh&   mesh    = EspNowMesh::instance();
static Miner&        miner   = Miner::instance();
static OledDisplay&  disp    = OledDisplay::instance();
static StratumClient stratum;

static bool     isMaster          = false;
static uint8_t  myMac[6]          = {0};
static uint8_t  currentJobId      = 0;
static uint32_t totalWorkerHR     = 0;
static uint32_t foundBlocks       = 0;
static uint32_t acceptedShares    = 0;   // persistent across pool reconnects
static uint32_t startupMs         = 0;
static uint32_t lastHeartbeatMs   = 0;
static uint32_t lastDisplayMs     = 0;
static uint32_t lastStatsMs       = 0;
static uint32_t masterLastSeenMs  = 0;
static bool     electionInProgress= false;
static uint32_t en2Counter        = 0;
// Cross-core nonce submission (miner runs Core 1, WiFi/ESP-NOW on Core 0)
static volatile bool     pendingNonceReady = false;
static volatile uint32_t pendingNonce      = 0;
static volatile uint8_t  pendingNonceJob   = 0;
static StratumJob lastStratumJob  = {};
static int      lastPeerCount     = 0;      // detect new worker joins
static uint32_t lastRebroadcastMs = 0;      // periodic job re-send to workers
static bool     _pendingRetry     = false;  // second dispatch ~800 ms after first
static uint32_t _retryMs          = 0;
static uint32_t lastPageFlipMs    = 0;      // auto page cycling
static uint32_t lastPoolRetryMs   = 0;      // non-blocking pool reconnect timer
static uint32_t lastLedMs         = 0;      // LED blink timer
static bool     ledState          = false;
static uint32_t lastBtnMs         = 0;      // button debounce
static bool     lastBtnState      = true;   // INPUT_PULLUP -> idle=HIGH

// ───────────────────────────────────────────────────────────────
//  SECTION 13 — JOB DISPATCH  (after all types are complete)
// ───────────────────────────────────────────────────────────────

static void buildCoinbase(uint8_t* out,uint16_t& outLen,
                           const StratumJob& sj,const StratumSession& sess,
                           uint32_t en2cnt)
{
    uint16_t pos=0;
    memcpy(out+pos,sj.coinbase1,sj.coinbase1_len); pos+=sj.coinbase1_len;
    memcpy(out+pos,sess.extranonce1,sess.extranonce1_len); pos+=sess.extranonce1_len;
    for(int i=0;i<sess.extranonce2_len&&i<4;i++) out[pos++]=(en2cnt>>(i*8))&0xFF;
    memcpy(out+pos,sj.coinbase2,sj.coinbase2_len); pos+=sj.coinbase2_len;
    outLen=pos;
}

static void dispatchJob(const StratumJob& sj){
    lastStratumJob=sj;
    en2Counter++;
    const StratumSession& sess=stratum.session();

    static uint8_t coinbase[300]; uint16_t cbLen=0;  // static avoids stack overflow
    buildCoinbase(coinbase,cbLen,sj,sess,en2Counter);
    uint8_t merkleRoot[32];
    stratum.computeMerkleRoot(merkleRoot,coinbase,cbLen);

    int workers=mesh.peerCount();
    int slots=workers+1;
    // Use 64-bit divide so result is correct for all slot counts (avoids overflow)
    uint32_t chunk=(uint32_t)(0x100000000ULL/(uint64_t)slots);
    if(chunk==0) chunk=0xFFFFFFFFUL;  // safety fallback

    // Build and broadcast job to each worker with its own nonce slice
    MeshJobMsg jm={};
    jm.msg_type=MSG_JOB; jm.job_id=++currentJobId;
    memcpy(jm.version,   sj.version,  4);
    memcpy(jm.prev_hash, sj.prev_hash,32);
    memcpy(jm.merkle_root,merkleRoot, 32);
    memcpy(jm.nbits,     sj.nbits,   4);
    memcpy(jm.ntime,     sj.ntime,   4);
    jm.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) jm.extranonce2[i]=(en2Counter>>(i*8))&0xFF;

    for(int i=0;i<workers;i++){
        PeerInfo peer; if(!mesh.getPeer(i,peer)) continue;
        jm.nonce_start=chunk*(uint32_t)(i+1);
        jm.nonce_end  =jm.nonce_start+chunk-1;
        jm.assigned_chunk=(uint8_t)(i+1);
        mesh.broadcastJob(jm);
    }

    // Master hashes chunk 0
    MinerJob mj={};
    mj.valid=true; mj.job_id=currentJobId;
    strncpy((char*)mj.stratum_job_id,sj.job_id,63);
    mj.nonce_start=0; mj.nonce_end=chunk-1;
    mj.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) mj.extranonce2[i]=(en2Counter>>(i*8))&0xFF;
    nbits_to_target(sj.nbits,mj.target);
    memcpy(mj.header,    sj.version,  4);
    memcpy(mj.header+4,  sj.prev_hash,32);
    memcpy(mj.header+36, merkleRoot,  32);
    memcpy(mj.header+68, sj.ntime,   4);
    memcpy(mj.header+72, sj.nbits,   4);
    miner.setJob(mj);

    Serial.printf("[Dispatch] job=%d  workers=%d  chunk=%08X\n",currentJobId,workers,chunk);
}

// Re-dispatch the current job to workers with fresh nonce slices.
// Called when a new worker joins or on the rebroadcast timer.
static void redispatchToWorkers(){
    if(!lastStratumJob.valid) return;
    int workers=mesh.peerCount();
    if(workers==0) return;
    int slots=workers+1;
    uint32_t chunk=(uint32_t)(0x100000000ULL/(uint64_t)slots);
    if(chunk==0) chunk=0xFFFFFFFFUL;

    MeshJobMsg jm={};
    jm.msg_type=MSG_JOB; jm.job_id=currentJobId;
    memcpy(jm.version,    lastStratumJob.version,  4);
    memcpy(jm.prev_hash,  lastStratumJob.prev_hash,32);
    const StratumSession& sess=stratum.session();
    static uint8_t coinbase[300]; uint16_t cbLen=0;  // static avoids stack overflow
    // reuse same en2Counter so workers hash complementary ranges
    uint16_t pos=0;
    memcpy(coinbase+pos,lastStratumJob.coinbase1,lastStratumJob.coinbase1_len); pos+=lastStratumJob.coinbase1_len;
    memcpy(coinbase+pos,sess.extranonce1,sess.extranonce1_len); pos+=sess.extranonce1_len;
    for(int i=0;i<sess.extranonce2_len&&i<4;i++) coinbase[pos++]=(en2Counter>>(i*8))&0xFF;
    memcpy(coinbase+pos,lastStratumJob.coinbase2,lastStratumJob.coinbase2_len); pos+=lastStratumJob.coinbase2_len;
    cbLen=pos;
    uint8_t merkleRoot[32];
    stratum.computeMerkleRoot(merkleRoot,coinbase,cbLen);

    memcpy(jm.merkle_root,merkleRoot,32);
    memcpy(jm.nbits,lastStratumJob.nbits,4);
    memcpy(jm.ntime,lastStratumJob.ntime,4);
    jm.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) jm.extranonce2[i]=(en2Counter>>(i*8))&0xFF;

    for(int i=0;i<workers;i++){
        PeerInfo peer; if(!mesh.getPeer(i,peer)) continue;
        jm.nonce_start=chunk*(uint32_t)(i+1);
        jm.nonce_end  =jm.nonce_start+chunk-1;
        jm.assigned_chunk=(uint8_t)(i+1);
        mesh.broadcastJob(jm);
        Serial.printf("[Redispatch] worker %d  nonce %08X-%08X\n",i+1,jm.nonce_start,jm.nonce_end);
    }

    // Shrink master's slice too
    MinerJob mj={};
    mj.valid=true; mj.job_id=currentJobId;
    strncpy((char*)mj.stratum_job_id,lastStratumJob.job_id,63);
    mj.nonce_start=0; mj.nonce_end=chunk-1;
    mj.extranonce2_len=sess.extranonce2_len;
    for(int i=0;i<4&&i<8;i++) mj.extranonce2[i]=(en2Counter>>(i*8))&0xFF;
    nbits_to_target(lastStratumJob.nbits,mj.target);
    memcpy(mj.header,    lastStratumJob.version,  4);
    memcpy(mj.header+4,  lastStratumJob.prev_hash,32);
    memcpy(mj.header+36, merkleRoot,              32);
    memcpy(mj.header+68, lastStratumJob.ntime,    4);
    memcpy(mj.header+72, lastStratumJob.nbits,    4);
    miner.setJob(mj);
    Serial.printf("[Redispatch] job=%d  workers=%d  chunk=%08X\n",currentJobId,workers,chunk);
}

// ───────────────────────────────────────────────────────────────
//  SECTION 14 — FALLBACK ELECTION
// ───────────────────────────────────────────────────────────────

static void checkElection(){
    if(isMaster||electionInProgress) return;
    uint32_t now=millis();
    if(masterLastSeenMs==0){masterLastSeenMs=now;return;}
    if(now-masterLastSeenMs>(uint32_t)(ELECTION_TRIGGER_COUNT*HEARTBEAT_INTERVAL_MS)){
        electionInProgress=true;
        Serial.println("[Election] master silent — electing");
        delay(random(0,ELECTION_BACKOFF_MAX_MS));
        MeshElectMsg e={};
        e.msg_type=MSG_ELECT; memcpy(e.candidate_mac,myMac,6);
        uint8_t pri=0; for(int i=0;i<6;i++) pri^=myMac[i];
        e.priority=pri;
        mesh.broadcastElection(e);
        delay(ELECTION_BACKOFF_MAX_MS);
        Serial.println("[Election] promoting to MASTER");
        isMaster=true;
        WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
        uint32_t t=millis();
        while(WiFi.status()!=WL_CONNECTED&&millis()-t<WIFI_TIMEOUT_MS) delay(200);
        if(WiFi.status()==WL_CONNECTED){
            stratum.onJob=[](const StratumJob& j){dispatchJob(j);};
            stratum.connect(POOL_HOST,POOL_PORT,POOL_USER,POOL_PASS);
            disp.showStatus("PROMOTED","Now master!");
        }
    }
}

// ───────────────────────────────────────────────────────────────
//  SECTION 15 — SETUP & LOOP
// ───────────────────────────────────────────────────────────────

// Actual WiFi channel — read after connect, ESP-NOW must match
static uint8_t g_wifiChannel = ESPNOW_CHANNEL;

static bool tryWifi(){
#if NODE_ROLE == ROLE_MASTER
    return true;
#elif NODE_ROLE == ROLE_WORKER
    return false;
#else
    Serial.printf("[Main] WiFi -> %s\n",WIFI_SSID);
    WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
    uint32_t t=millis();
    while(WiFi.status()!=WL_CONNECTED&&millis()-t<WIFI_TIMEOUT_MS){delay(200);Serial.print(".");}
    Serial.println();
    if(WiFi.status()==WL_CONNECTED){
        // Read the real channel the AP assigned — ESP-NOW MUST use this same channel
        wifi_second_chan_t second;
        esp_wifi_get_channel(&g_wifiChannel,&second);
        Serial.printf("[Main] IP: %s  WiFi channel: %d\n",
            WiFi.localIP().toString().c_str(),(int)g_wifiChannel);
        return true;
    }
    return false;
#endif
}

void setup(){
    Serial.begin(SERIAL_BAUD);
    delay(100);
    Serial.println("\n=== MeshMiner 32 ===");
    startupMs=millis();
    WiFi.macAddress(myMac);
    Serial.printf("[Main] MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
        myMac[0],myMac[1],myMac[2],myMac[3],myMac[4],myMac[5]);

    disp.begin();
    delay(1500);   // show splash

    isMaster=tryWifi();

    if(isMaster){
        // ── MASTER ────────────────────────────
        Serial.println("[Main] role=MASTER");
        disp.showStatus("MASTER","Mesh init...");
        mesh.begin(true, g_wifiChannel);

        stratum.onJob=[](const StratumJob& j){
            dispatchJob(j);
            // If workers were already seen before the first job arrived, send them work now
            if(mesh.peerCount()>0){
                Serial.printf("[Main] job arrived with %d workers waiting — redispatching\n",mesh.peerCount());
                redispatchToWorkers();
                lastRebroadcastMs=millis();
            }
        };

        mesh.onResult=[](const MeshResultMsg& r){
            Serial.printf("[Master] worker result nonce=%08X job=%d\n",r.nonce,r.job_id);
            if(lastStratumJob.valid){
                uint8_t en2[8]={};
                for(int i=0;i<4;i++) en2[i]=(en2Counter>>(i*8))&0xFF;
                stratum.submit(lastStratumJob,r.nonce,en2,stratum.session().extranonce2_len);
            }
            foundBlocks++;
        };

        mesh.onStats=[](const MeshStatsMsg&){
            totalWorkerHR=0;
            int cnt=mesh.peerCount();
            for(int i=0;i<cnt;i++){PeerInfo p;if(mesh.getPeer(i,p))totalWorkerHR+=p.hash_rate;}
        };

        miner.onFound=[](const MinerJob& job,uint32_t nonce){
            // DO NOT call WiFi/stratum here — we are on Core 1
            // Signal Core 0 via volatile flag to submit safely
            pendingNonce    = nonce;
            pendingNonceJob = job.job_id;
            pendingNonceReady = true;
            foundBlocks++;
        };

        disp.showStatus("MASTER","Connecting pool...");
        if(!stratum.connect(POOL_HOST,POOL_PORT,POOL_USER,POOL_PASS)){
            disp.showStatus("Pool FAILED","check POOL_USER");
            Serial.println("[Main] Pool connect failed — will retry in loop");
        } else {
            disp.showStatus("MASTER","Pool connected!");
        }

    } else {
        // ── WORKER ────────────────────────────
        Serial.println("[Main] role=WORKER");
        disp.showStatus("WORKER","Listening...");
        mesh.begin(false);

        mesh.onJob=[](const MeshJobMsg& j){
            Serial.printf("[Worker] job=%d  nonce %08X-%08X\n",j.job_id,j.nonce_start,j.nonce_end);
            MinerJob mj={};
            mj.valid=true; mj.job_id=j.job_id;
            mj.nonce_start=j.nonce_start; mj.nonce_end=j.nonce_end;
            mj.extranonce2_len=j.extranonce2_len;
            memcpy(mj.extranonce2,j.extranonce2,j.extranonce2_len);
            nbits_to_target(j.nbits,mj.target);
            memcpy(mj.header,    j.version,    4);
            memcpy(mj.header+4,  j.prev_hash,  32);
            memcpy(mj.header+36, j.merkle_root,32);
            memcpy(mj.header+68, j.ntime,      4);
            memcpy(mj.header+72, j.nbits,      4);
            miner.setJob(mj);
        };

        miner.onFound=[](const MinerJob& job,uint32_t nonce){
            // Signal Core 0 via flag — esp_now_send is not safe from Core 1
            pendingNonce    = nonce;
            pendingNonceJob = job.job_id;
            pendingNonceReady = true;
            foundBlocks++;
        };

        mesh.onHeartbeat=[](const MeshHeartbeatMsg& hb){
            if(hb.is_master){masterLastSeenMs=millis();electionInProgress=false;}
        };
    }

    miner.begin();

    // BOOT button (GPIO 0) for manual page flip — INPUT_PULLUP
    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
    // Blue LED — blinks at hash speed
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    lastPageFlipMs = millis();   // prevent instant flip on first loop tick
    disp.setPage(PAGE_MINING);   // always start on the main mining page

    Serial.println("[Main] setup complete");
}

void loop(){
    uint32_t now=millis();

    // Cross-core nonce submission — handle found nonce from miner (Core 1) safely on Core 0
    if(pendingNonceReady){
        pendingNonceReady=false;
        uint32_t nonce = pendingNonce;
        Serial.printf("[Found] nonce=%08X job=%d\n", nonce, (int)pendingNonceJob);
        if(isMaster){
            if(lastStratumJob.valid){
                uint8_t en2[8]={};
                for(int i=0;i<4;i++) en2[i]=(en2Counter>>(i*8))&0xFF;
                stratum.submit(lastStratumJob,nonce,en2,stratum.session().extranonce2_len);
            }
        } else {
            uint8_t masterMac[6];
            if(mesh.getMasterMac(masterMac)){
                MeshResultMsg r={};
                r.msg_type=MSG_RESULT; r.job_id=pendingNonceJob; r.nonce=nonce;
                memcpy(r.worker_mac,myMac,6);
                mesh.sendResult(masterMac,r);
            }
        }
    }

    // Pool keep-alive + receive (master only)
    if(isMaster){
        if(!stratum.isConnected()){
            // Non-blocking retry — don't block the loop for 10s every tick
            if(now-lastPoolRetryMs>=POOL_RETRY_MS){
                lastPoolRetryMs=now;
                Serial.println("[Main] pool reconnecting...");
                stratum.connect(POOL_HOST,POOL_PORT,POOL_USER,POOL_PASS);
            }
        } else {
            stratum.loop();
        }
    }

    // Heartbeat broadcast
    if(now-lastHeartbeatMs>=HEARTBEAT_INTERVAL_MS){
        lastHeartbeatMs=now;
        MeshHeartbeatMsg hb={};
        hb.msg_type=MSG_HEARTBEAT;
        memcpy(hb.sender_mac,myMac,6);
        hb.is_master=isMaster?1:0;
        hb.uptime_s=(now-startupMs)/1000;
        hb.total_hash_rate=isMaster?totalWorkerHR+miner.getStats().hash_rate:miner.getStats().hash_rate;
        mesh.broadcastHeartbeat(hb);
        mesh.removeStalePeers(now);
    }

    // New worker joined — immediately re-slice and send jobs
    if(isMaster){
        int curPeers=mesh.peerCount();
        // _newPeer flag is set inside the ESP-NOW ISR when addPeer() registers a new node
        if(mesh.takeNewPeer()){
            lastPeerCount=curPeers;
            Serial.printf("[Main] new peer -> %d workers\n",curPeers);
            if(lastStratumJob.valid){
                // Job already known — dispatch immediately
                Serial.println("[Main] job known, dispatching now");
                redispatchToWorkers();
                lastRebroadcastMs=now;
            } else {
                // Job not yet received — onJob callback will dispatch when it arrives
                Serial.println("[Main] waiting for first job from pool...");
            }
            // Always schedule a retry 1.5 s later as belt-and-braces
            _pendingRetry = true;
            _retryMs      = now + 1500;
        }
        // Retry dispatch ~800 ms after a new peer joined (belt-and-braces)
        if(_pendingRetry && now>=_retryMs){
            _pendingRetry=false;
            Serial.println("[Main] retry dispatch to new worker");
            redispatchToWorkers();
            lastRebroadcastMs=now;
        }
        // Periodic rebroadcast every 5 s — catches workers that reset or missed a packet
        if(lastStratumJob.valid && curPeers>0 && now-lastRebroadcastMs>=60000){
            lastRebroadcastMs=now;
            Serial.println("[Main] periodic rebroadcast");
            redispatchToWorkers();
        }
    }

    // Worker stats + election check
    if(!isMaster&&now-lastStatsMs>=5000){
        lastStatsMs=now;
        uint8_t masterMac[6];
        if(mesh.getMasterMac(masterMac)){
            MeshStatsMsg s={};
            s.msg_type=MSG_STATS;
            memcpy(s.worker_mac,myMac,6);
            s.hash_rate    =miner.getStats().hash_rate;
            s.nonces_tested=miner.getStats().total_hashes;
            s.job_id       =currentJobId;
            mesh.sendStats(masterMac,s);
        }
        checkElection();
    }

    // ── Display: page flip + refresh (unified block) ───────────
    {
        // Button check — BOOT button active LOW
        bool btn=digitalRead(BOOT_BTN_PIN);
        if(!btn && lastBtnState && (now-lastBtnMs)>200){
            lastBtnMs=now;
            disp.nextPage();
            lastDisplayMs=0;     // force redraw immediately
            lastPageFlipMs=now;  // reset auto-flip timer
            Serial.println("[Display] btn flip");
        }
        lastBtnState=btn;
    }

    // Display refresh + auto page flip
    if(now-lastDisplayMs>=OLED_REFRESH_MS){
        lastDisplayMs=now;
        MinerStats ms=miner.getStats();
        DisplayData dd={};
        dd.hash_rate       =isMaster?totalWorkerHR+ms.hash_rate:ms.hash_rate;
        dd.total_hashes    =ms.total_hashes;
        dd.found_blocks    =foundBlocks;
        // Accumulate accepted shares — persists across reconnects
        {
            static uint32_t lastAcc=0;
            uint32_t curAcc=stratum.acceptedShares();
            if(curAcc>lastAcc){ acceptedShares+=curAcc-lastAcc; lastAcc=curAcc; }
        }
        dd.accepted_shares = acceptedShares;
        dd.is_master       =isMaster;
        dd.uptime_s        =(now-startupMs)/1000;
        dd.peer_count      =mesh.peerCount();
        strncpy(dd.pool_host,POOL_HOST,sizeof(dd.pool_host)-1);
        dd.pool_port       =POOL_PORT;
        dd.difficulty      =isMaster?stratum.session().difficulty:0;
        if(WiFi.status()==WL_CONNECTED){
            strncpy(dd.ssid,WiFi.SSID().c_str(),sizeof(dd.ssid)-1);
            strncpy(dd.ip,WiFi.localIP().toString().c_str(),sizeof(dd.ip)-1);
            dd.rssi=WiFi.RSSI();
        }
        int cnt=min(mesh.peerCount(),8);
        for(int i=0;i<cnt;i++){
            PeerInfo p;if(mesh.getPeer(i,p)){memcpy(dd.worker_macs[i],p.mac,6);dd.worker_hash_rates[i]=p.hash_rate;}
        }
        if(strlen(lastStratumJob.job_id)) strncpy(dd.job_id,lastStratumJob.job_id,15);
        dd.mining_active = (ms.total_hashes > 0);
        dd.tick          = (uint32_t)(now / 500);

        // Auto page flip — advance page every PAGE_FLIP_MS
        if(now-lastPageFlipMs>=PAGE_FLIP_MS){
            lastPageFlipMs=now;
            disp.nextPage();
            Serial.printf("[Display] auto -> page %d\n", (int)((now/PAGE_FLIP_MS)%PAGE_COUNT));
        }

        disp.update(dd);
    }

    // Blue LED blinks at hash speed — faster = more hashes
    // At 30kH/s: blink every ~33ms. Cap at 50ms min so it's visible.
    {
        uint32_t hr = miner.getStats().hash_rate;
        uint32_t blinkInterval = (hr > 0) ? max(50UL, 1000000UL/hr) : 500;
        if(now-lastLedMs >= blinkInterval){
            lastLedMs=now;
            ledState=!ledState;
            digitalWrite(LED_PIN, ledState ? HIGH : LOW);
        }
    }

    delay(10);
}
