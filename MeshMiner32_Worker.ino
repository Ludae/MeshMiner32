/*
 * ═══════════════════════════════════════════════════════════════
 *  MeshMiner32 Edition  —  WORKER node
 *  Target  : ESP32 DevKit V1  (esp32 board package 3.x)
 *
 *  No WiFi, no pool connection needed on workers.
 *  Workers receive jobs from the master over ESP-NOW,
 *  hash their assigned nonce range, and report results back.
 *
 *  Libraries (install via Arduino Library Manager):
 * *
 *  SPI OLED wiring (same as master):
 *  ┌───────────┬─────────────────────────────┐
 *  │ OLED pin  │ ESP32 pin                   │
 *  ├───────────┼─────────────────────────────┤
 *  │ VCC/3V3   │ 3.3V                        │
 *  │ GND       │ GND                         │
 *  │ SCL       │ GPIO 18  (HW SPI SCK, D18)  │
 *  │ SDA       │ GPIO 23  (HW SPI MOSI, D23) │
 *  │ RES       │ GPIO 4   (D4)               │
 *  │ DC        │ GPIO 22  (D22)              │
 *  │ CS        │ GND on module (no wire)     │
 *  └───────────┴─────────────────────────────┘
 *
 *  Flash this sketch to every ESP32 that acts as a worker.
 *  Flash MeshMiner32.ino to the master (the one with WiFi).
 *  All boards must be on the same ESP-NOW channel (default 1).
 * ═══════════════════════════════════════════════════════════════
 */

// ───────────────────────────────────────────────────────────────
//  SECTION 1 — CONFIGURATION
// ───────────────────────────────────────────────────────────────

// Blue LED pin — blinks at hash speed (GPIO 2 = built-in LED on most DevKit V1)
#define LED_PIN  2

// ESP-NOW channel — must match the WiFi channel your router uses.
// Your master printed "WiFi channel: 11" — so set 11 here.
// If you change routers, check the master Serial output and update this.
#define ESPNOW_CHANNEL  11

// OLED SPI pins (no CS — tied to GND on module)
// SCL -> D18 (GPIO 18, HW SPI SCK)
// SDA -> D23 (GPIO 23, HW SPI MOSI)


// Mining task — dual core: Core 0 and Core 1 each get half the nonce range
#define MINING_TASK_PRIORITY  5
#define MINING_TASK_STACK     8192

// Mesh timing
#define MESH_MAX_PEERS          20
#define MESH_FRAME_LEN          250
#define PEER_TIMEOUT_MS         15000
#define HEARTBEAT_INTERVAL_MS   5000
#define ELECTION_TRIGGER_COUNT  3
#define ELECTION_BACKOFF_MAX_MS 2000

// Stats report interval (ms)
#define STATS_INTERVAL_MS  5000

#define SERIAL_BAUD  115200

// ───────────────────────────────────────────────────────────────
//  SECTION 2 — INCLUDES
// ───────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <functional>
#include <string.h>

// ───────────────────────────────────────────────────────────────
//  SECTION 3 — FORWARD DECLARATIONS
// ───────────────────────────────────────────────────────────────

struct MinerJob;
struct MinerStats;
struct PeerInfo;
static void checkElection();

// ───────────────────────────────────────────────────────────────
//  SECTION 4 — SHA-256  (clean rewrite, plain software)
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

// Double-SHA256 of arbitrary-length input
static void double_sha256(const uint8_t* data,size_t len,uint8_t* digest){
    uint8_t mid[32];
    // First SHA-256
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
    // Second SHA-256 (of 32-byte inner hash)
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
//  SECTION 5 — ESP-NOW MESSAGE STRUCTS
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
//  SECTION 6 — ESP-NOW LAYER  (worker-only: no broadcast job)
// ───────────────────────────────────────────────────────────────

class EspNowWorker {
public:
    static EspNowWorker& instance(){ static EspNowWorker i; return i; }

    // channel: set this to match your router's WiFi channel.
    // The master prints "WiFi channel: N" on Serial — use that number.
    // Default 0 = auto-detect by scanning for the master's beacon.
    bool begin(uint8_t channel=ESPNOW_CHANNEL){
        _channel=channel;
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        // Workers have no WiFi connection, so we set the channel explicitly.
        // It must match whatever channel the master's router is on.
        esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
        Serial.printf("[Mesh] worker ESP-NOW channel: %d\n",(int)_channel);

        if(esp_now_init()!=ESP_OK){ Serial.println("[Mesh] init failed"); return false; }
        esp_now_register_recv_cb(_recv_cb);
        esp_now_register_send_cb(_send_cb);

        // Broadcast peer — receive jobs from any master address
        esp_now_peer_info_t bc={};
        memcpy(bc.peer_addr,ESPNOW_BROADCAST,6);
        bc.channel=_channel; bc.encrypt=false;
        esp_now_add_peer(&bc);

        _initialized=true;
        Serial.println("[Mesh] worker ready");
        return true;
    }

    bool sendResult(const MeshResultMsg& r){
        if(!_hasMaster) return false;
        return _tx(_masterMac,&r,sizeof(r));
    }

    bool sendStats(const MeshStatsMsg& s){
        if(!_hasMaster) return false;
        return _tx(_masterMac,&s,sizeof(s));
    }

    bool sendHeartbeat(const MeshHeartbeatMsg& h){
        return _tx(ESPNOW_BROADCAST,&h,sizeof(h));
    }

    bool sendElection(const MeshElectMsg& e){
        return _tx(ESPNOW_BROADCAST,&e,sizeof(e));
    }

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
        Serial.printf("[Mesh] +peer %02X:%02X:%02X:%02X:%02X:%02X\n",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        return true;
    }

    void setMasterMac(const uint8_t* mac){
        memcpy(_masterMac,mac,6); _hasMaster=true; addPeer(mac);
        Serial.printf("[Mesh] master=%02X:%02X:%02X:%02X:%02X:%02X\n",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    }

    bool getMasterMac(uint8_t* out) const {
        if(!_hasMaster) return false; memcpy(out,_masterMac,6); return true;
    }

    bool     hasMaster()    const { return _hasMaster; }
    uint32_t masterLastSeen() const { return _masterLastSeen; }

    // Callbacks
    std::function<void(const MeshJobMsg&)>        onJob;
    std::function<void(const MeshHeartbeatMsg&)>  onHeartbeat;
    std::function<void(const MeshElectMsg&)>      onElect;

private:
    EspNowWorker()=default;

    bool _tx(const uint8_t* mac, const void* data, size_t len){
        if(!_initialized||len>MESH_FRAME_LEN) return false;
        uint8_t frame[MESH_FRAME_LEN]={0}; memcpy(frame,data,len);
        return esp_now_send(mac,frame,MESH_FRAME_LEN)==ESP_OK;
    }

    static void _recv_cb(const esp_now_recv_info_t* info, const uint8_t* data, int len){
        if(len<1||!info) return;
        const uint8_t* mac=info->src_addr;
        EspNowWorker& w=instance();
        uint8_t type=data[0];

        switch(type){
            case MSG_JOB: {
                if(len<(int)sizeof(MeshJobMsg)) return;
                MeshJobMsg j; memcpy(&j,data,sizeof(j));
                // Auto-register sender as master if we don't have one
                if(!w._hasMaster) w.setMasterMac(mac);
                if(w.onJob) w.onJob(j);
                break;
            }
            case MSG_HEARTBEAT: {
                if(len<(int)sizeof(MeshHeartbeatMsg)) return;
                MeshHeartbeatMsg h; memcpy(&h,data,sizeof(h));
                if(h.is_master){
                    if(!w._hasMaster) w.setMasterMac(mac);
                    w._masterLastSeen=millis();
                }
                w.addPeer(mac);
                if(w.onHeartbeat) w.onHeartbeat(h);
                break;
            }
            case MSG_ELECT: {
                if(len<(int)sizeof(MeshElectMsg)) return;
                MeshElectMsg e; memcpy(&e,data,sizeof(e));
                if(w.onElect) w.onElect(e);
                break;
            }
            default: break;
        }
    }

    static void _send_cb(const wifi_tx_info_t*, esp_now_send_status_t){}

    PeerInfo _peers[MESH_MAX_PEERS];
    int      _peerCount      = 0;
    bool     _initialized    = false;
    bool     _hasMaster      = false;
    uint8_t  _masterMac[6]   = {0};
    uint32_t _masterLastSeen = 0;
    uint8_t  _channel        = ESPNOW_CHANNEL;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 7 — MINER  (single-core, Core 1)
// ───────────────────────────────────────────────────────────────

struct MinerJob {
    uint8_t  header[80];
    uint8_t  target[32];
    uint32_t nonce_start;
    uint32_t nonce_end;
    uint8_t  job_id;
    bool     valid;
};

struct MinerStats {
    uint32_t hash_rate;
    uint32_t total_hashes;
    uint32_t found_nonces;
};

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
        // Core 0: lower half of nonce range
        xTaskCreatePinnedToCore(_task,"mineC0",MINING_TASK_STACK,&_ctx[0],
            MINING_TASK_PRIORITY,nullptr,0);
        // Core 1: upper half of nonce range
        xTaskCreatePinnedToCore(_task,"mineC1",MINING_TASK_STACK,&_ctx[1],
            MINING_TASK_PRIORITY,nullptr,1);
        Serial.println("[Miner] dual-core tasks started");
    }

    void setJob(const MinerJob& j){
        // Split nonce range evenly between cores
        uint32_t half = j.nonce_start + (j.nonce_end - j.nonce_start) / 2;
        MinerJob j0=j; j0.nonce_end   = half;
        MinerJob j1=j; j1.nonce_start = half;
        for(int c=0;c<2;c++){
            MinerJob& jc=(c==0)?j0:j1;
            if(!_ctx[c].mutex) continue;
            xSemaphoreTake(_ctx[c].mutex, portMAX_DELAY);
            _ctx[c].pending = jc;
            _ctx[c].newJob  = true;
            xSemaphoreGive(_ctx[c].mutex);
        }
    }

    MinerStats getStats() const {
        return {_ctx[0].hashRate+_ctx[1].hashRate,
                _ctx[0].totalH  +_ctx[1].totalH,
                _ctx[0].found   +_ctx[1].found};
    }

    std::function<void(const MinerJob&, uint32_t)> onFound;

private:
    Miner()=default;
    CoreCtx _ctx[2];

    static void _task(void* p){ _run(static_cast<CoreCtx*>(p)); vTaskDelete(nullptr); }

    static void _run(CoreCtx* ctx){
        uint8_t   hash[32];
        uint32_t  cnt=0, wstart=millis();
        MinerJob  job={};

        while(true){
            if(ctx->newJob && ctx->mutex && xSemaphoreTake(ctx->mutex,0)==pdTRUE){
                job         = ctx->pending;
                ctx->newJob = false;
                cnt         = 0;
                wstart      = millis();
                xSemaphoreGive(ctx->mutex);
                Serial.printf("[Miner%d] job=%d  %08lX-%08lX\n",
                    xPortGetCoreID(),(int)job.job_id,
                    (unsigned long)job.nonce_start,(unsigned long)job.nonce_end);
            }

            if(!job.valid){ vTaskDelay(pdMS_TO_TICKS(50)); continue; }

            for(uint32_t n=job.nonce_start; n<job.nonce_end && !ctx->newJob; n++){
                job.header[76]= n       & 0xFF;
                job.header[77]=(n>>  8) & 0xFF;
                job.header[78]=(n>> 16) & 0xFF;
                job.header[79]=(n>> 24) & 0xFF;
                bitcoin_hash(job.header, hash);
                cnt++;
                if(check_difficulty(hash, job.target)){
                    ctx->found++;
                    Miner::instance()._notifyFound(job, n);
                }
                if((cnt & 0x3FF)==0){
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
//  SECTION 9 — GLOBAL STATE
// ───────────────────────────────────────────────────────────────

static EspNowWorker& mesh  = EspNowWorker::instance();
static Miner&        miner = Miner::instance();

static uint8_t  myMac[6]          = {0};
static uint8_t  currentJobId      = 0;
static uint32_t startupMs         = 0;
static uint32_t lastHeartbeatMs   = 0;
static uint32_t lastStatsMs       = 0;
static uint32_t masterLastSeenMs  = 0;
static uint32_t lastLedMs         = 0;
static bool     ledState          = false;
static bool     electionInProgress= false;
static uint32_t currentNonceStart   = 0;
static uint32_t currentNonceEnd     = 0;
// Cross-core nonce flag (miner=Core1, ESP-NOW=Core0)
static volatile bool     pendingNonceReady = false;
static volatile uint32_t pendingNonce      = 0;
static volatile uint8_t  pendingNonceJob   = 0;

// ───────────────────────────────────────────────────────────────
//  SECTION 10 — ELECTION LOGIC
// ───────────────────────────────────────────────────────────────

// If the master goes silent, workers elect a new one.
// Lowest XOR-of-MAC priority wins. The winner would need WiFi
// credentials to actually connect to the pool — this is a
// best-effort failover.  Set NODE_ROLE ROLE_MASTER on the
// intended backup node and flash MeshMiner32.ino to it instead
// for a fully capable promotion.
static void checkElection(){
    if(electionInProgress) return;
    uint32_t now=millis();
    if(masterLastSeenMs==0){ masterLastSeenMs=now; return; }
    if(now-masterLastSeenMs > (uint32_t)(ELECTION_TRIGGER_COUNT*HEARTBEAT_INTERVAL_MS)){
        electionInProgress=true;
        Serial.println("[Election] master silent — broadcasting candidacy");
        delay(random(0, ELECTION_BACKOFF_MAX_MS));

        MeshElectMsg e={};
        e.msg_type=MSG_ELECT;
        memcpy(e.candidate_mac,myMac,6);
        uint8_t pri=0; for(int i=0;i<6;i++) pri^=myMac[i];
        e.priority=pri;
        mesh.sendElection(e);

        // After back-off: update display to show master is lost
            Serial.println("[Election] candidate broadcast sent");
        // Election resolution happens on whichever node runs the master sketch
    }
}

// ───────────────────────────────────────────────────────────────
//  SECTION 11 — SETUP & LOOP
// ───────────────────────────────────────────────────────────────

void setup(){
    Serial.begin(SERIAL_BAUD);
    delay(100);
    Serial.println("\n=== MeshMiner 32 WORKER ===");

    // ── Raw SHA benchmark (before WiFi/tasks — pure compute measurement) ──
    {
        uint8_t hdr[80]={0}; uint8_t out[32];
        for(int i=0;i<100;i++) double_sha256(hdr,80,out);  // warm-up
        uint32_t t0=micros();
        for(int i=0;i<2000;i++) double_sha256(hdr,80,out);
        uint32_t us=micros()-t0;
        uint32_t hps=(uint32_t)(2000000000ULL/us);
        Serial.printf("[Bench] 2000 hashes in %lu us = %lu H/s raw\n",
            (unsigned long)us,(unsigned long)hps);
        Serial.printf("[Bench] CPU %lu MHz\n",(unsigned long)getCpuFrequencyMhz());
    }

    startupMs=millis();
    WiFi.macAddress(myMac);
    Serial.printf("[Worker] MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
        myMac[0],myMac[1],myMac[2],myMac[3],myMac[4],myMac[5]);


    mesh.begin();

    // ── Job received from master ──────────────────────────────
    mesh.onJob=[](const MeshJobMsg& j){
        Serial.printf("[Worker] job=%d  nonce %08lX-%08lX  chunk=%d\n",
            j.job_id,(unsigned long)j.nonce_start,
            (unsigned long)j.nonce_end, j.assigned_chunk);

        currentJobId      = j.job_id;
        currentNonceStart = j.nonce_start;
        currentNonceEnd   = j.nonce_end;
        electionInProgress= false;   // master is alive

        MinerJob mj={};
        mj.valid      = true;
        mj.job_id     = j.job_id;
        mj.nonce_start= j.nonce_start;
        mj.nonce_end  = j.nonce_end;
        difficulty_to_target(j.pool_difficulty, mj.target);  // pool share diff, NOT nbits
        memcpy(mj.header,     j.version,    4);
        memcpy(mj.header+4,   j.prev_hash,  32);
        memcpy(mj.header+36,  j.merkle_root,32);
        memcpy(mj.header+68,  j.ntime,      4);
        memcpy(mj.header+72,  j.nbits,      4);
        miner.setJob(mj);
    };

    // ── Heartbeat from master ────────────────────────────────
    mesh.onHeartbeat=[](const MeshHeartbeatMsg& hb){
        if(hb.is_master){
            masterLastSeenMs   = millis();
            electionInProgress = false;
        }
    };

    // ── Election message received ────────────────────────────
    mesh.onElect=[](const MeshElectMsg& e){
        // Compare with our own priority; lower wins.
        // A full election protocol would track all candidates —
        // for simplicity we just log and let the master-sketch
        // node handle the actual promotion.
        uint8_t myPri=0; for(int i=0;i<6;i++) myPri^=myMac[i];
        Serial.printf("[Election] candidate %02X:%02X:%02X pri=%d (mine=%d)\n",
            e.candidate_mac[3],e.candidate_mac[4],e.candidate_mac[5],
            e.priority, myPri);
    };

    // ── Nonce found — report to master ──────────────────────
    miner.onFound=[](const MinerJob& job, uint32_t nonce){
        // Do NOT call esp_now_send from Core 1 — flag for Core 0 to handle
        pendingNonce     = nonce;
        pendingNonceJob  = job.job_id;
        pendingNonceReady= true;
    };

    // Blue LED — blinks at hash speed
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    miner.begin();

    // Send first heartbeat immediately so master detects us right away
    // (instead of waiting up to HEARTBEAT_INTERVAL_MS = 5 s)
    {
        MeshHeartbeatMsg hb={};
        hb.msg_type=MSG_HEARTBEAT;
        memcpy(hb.sender_mac,myMac,6);
        hb.is_master=0; hb.uptime_s=0; hb.total_hash_rate=0;
        mesh.sendHeartbeat(hb);
        lastHeartbeatMs=millis();
        Serial.println("[Worker] initial heartbeat sent");
    }

    Serial.println("[Worker] setup complete");
}

void loop(){
    uint32_t now=millis();

    // Handle found nonce safely on Core 0
    if(pendingNonceReady){
        pendingNonceReady=false;
        Serial.printf("[Worker] *** FOUND nonce=%08lX ***\n",(unsigned long)pendingNonce);
        uint8_t masterMac[6];
        if(mesh.getMasterMac(masterMac)){
            MeshResultMsg r={};
            r.msg_type=MSG_RESULT;
            r.job_id  =(uint8_t)pendingNonceJob;
            r.nonce   =pendingNonce;
            memcpy(r.worker_mac,myMac,6);
            mesh.sendResult(r);
        } else {
            Serial.println("[Worker] no master MAC for result");
        }
    }

    // ── Heartbeat broadcast (so master knows we exist) ───────
    if(now-lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS){
        lastHeartbeatMs=now;
        MeshHeartbeatMsg hb={};
        hb.msg_type       = MSG_HEARTBEAT;
        memcpy(hb.sender_mac, myMac, 6);
        hb.is_master      = 0;
        hb.uptime_s       = (now-startupMs)/1000;
        hb.total_hash_rate= miner.getStats().hash_rate;
        mesh.sendHeartbeat(hb);
    }

    // ── Periodic stats report to master ─────────────────────
    if(now-lastStatsMs >= STATS_INTERVAL_MS){
        lastStatsMs=now;
        if(mesh.hasMaster()){
            MinerStats ws=miner.getStats();
            MeshStatsMsg s={};
            s.msg_type     = MSG_STATS;
            memcpy(s.worker_mac, myMac, 6);
            s.hash_rate    = ws.hash_rate;
            s.nonces_tested= ws.total_hashes;
            s.job_id       = currentJobId;
            mesh.sendStats(s);
            Serial.printf("[Worker] %lu H/s  hashes=%lu  found=%lu\n",
                (unsigned long)ws.hash_rate,
                (unsigned long)ws.total_hashes,
                (unsigned long)ws.found_nonces);
        }
        checkElection();
    }


    // Blue LED blinks proportional to hash rate
    {
        uint32_t hr = miner.getStats().hash_rate;
        uint32_t blinkInterval = (hr > 0) ? max(50UL, 1000000UL/hr) : 500;
        if(now - lastLedMs >= blinkInterval){
            lastLedMs = now;
            ledState  = !ledState;
            digitalWrite(LED_PIN, ledState ? HIGH : LOW);
        }
    }

    delay(10);
}
/*
 * ═══════════════════════════════════════════════════════════════
 *  MeshMiner32 Edition  —  WORKER node
 *  Target  : ESP32 DevKit V1  (esp32 board package 3.x)
 *
 *  No WiFi, no pool connection needed on workers.
 *  Workers receive jobs from the master over ESP-NOW,
 *  hash their assigned nonce range, and report results back.
 *
 *  Libraries (install via Arduino Library Manager):
 * *
 *  SPI OLED wiring (same as master):
 *  ┌───────────┬─────────────────────────────┐
 *  │ OLED pin  │ ESP32 pin                   │
 *  ├───────────┼─────────────────────────────┤
 *  │ VCC/3V3   │ 3.3V                        │
 *  │ GND       │ GND                         │
 *  │ SCL       │ GPIO 18  (HW SPI SCK, D18)  │
 *  │ SDA       │ GPIO 23  (HW SPI MOSI, D23) │
 *  │ RES       │ GPIO 4   (D4)               │
 *  │ DC        │ GPIO 22  (D22)              │
 *  │ CS        │ GND on module (no wire)     │
 *  └───────────┴─────────────────────────────┘
 *
 *  Flash this sketch to every ESP32 that acts as a worker.
 *  Flash MeshMiner32.ino to the master (the one with WiFi).
 *  All boards must be on the same ESP-NOW channel (default 1).
 * ═══════════════════════════════════════════════════════════════
 */

// ───────────────────────────────────────────────────────────────
//  SECTION 1 — CONFIGURATION
// ───────────────────────────────────────────────────────────────

// Blue LED pin — blinks at hash speed (GPIO 2 = built-in LED on most DevKit V1)
#define LED_PIN  2

// ESP-NOW channel — must match the WiFi channel your router uses.
// Your master printed "WiFi channel: 11" — so set 11 here.
// If you change routers, check the master Serial output and update this.
#define ESPNOW_CHANNEL  11

// OLED SPI pins (no CS — tied to GND on module)
// SCL -> D18 (GPIO 18, HW SPI SCK)
// SDA -> D23 (GPIO 23, HW SPI MOSI)


// Mining task
#define MINING_TASK_PRIORITY  5
#define MINING_TASK_CORE      1
#define MINING_TASK_STACK     8192

// Mesh timing
#define MESH_MAX_PEERS          20
#define MESH_FRAME_LEN          250
#define PEER_TIMEOUT_MS         15000
#define HEARTBEAT_INTERVAL_MS   5000
#define ELECTION_TRIGGER_COUNT  3
#define ELECTION_BACKOFF_MAX_MS 2000

// Stats report interval (ms)
#define STATS_INTERVAL_MS  5000

#define SERIAL_BAUD  115200

// ───────────────────────────────────────────────────────────────
//  SECTION 2 — INCLUDES
// ───────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <functional>
#include <string.h>

// ───────────────────────────────────────────────────────────────
//  SECTION 3 — FORWARD DECLARATIONS
// ───────────────────────────────────────────────────────────────

struct MinerJob;
struct MinerStats;
struct PeerInfo;
static void checkElection();

// ───────────────────────────────────────────────────────────────
//  SECTION 4 — SHA-256
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
//  SECTION 5 — ESP-NOW MESSAGE STRUCTS
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
//  SECTION 6 — ESP-NOW LAYER  (worker-only: no broadcast job)
// ───────────────────────────────────────────────────────────────

class EspNowWorker {
public:
    static EspNowWorker& instance(){ static EspNowWorker i; return i; }

    // channel: set this to match your router's WiFi channel.
    // The master prints "WiFi channel: N" on Serial — use that number.
    // Default 0 = auto-detect by scanning for the master's beacon.
    bool begin(uint8_t channel=ESPNOW_CHANNEL){
        _channel=channel;
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        // Workers have no WiFi connection, so we set the channel explicitly.
        // It must match whatever channel the master's router is on.
        esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
        Serial.printf("[Mesh] worker ESP-NOW channel: %d\n",(int)_channel);

        if(esp_now_init()!=ESP_OK){ Serial.println("[Mesh] init failed"); return false; }
        esp_now_register_recv_cb(_recv_cb);
        esp_now_register_send_cb(_send_cb);

        // Broadcast peer — receive jobs from any master address
        esp_now_peer_info_t bc={};
        memcpy(bc.peer_addr,ESPNOW_BROADCAST,6);
        bc.channel=_channel; bc.encrypt=false;
        esp_now_add_peer(&bc);

        _initialized=true;
        Serial.println("[Mesh] worker ready");
        return true;
    }

    bool sendResult(const MeshResultMsg& r){
        if(!_hasMaster) return false;
        return _tx(_masterMac,&r,sizeof(r));
    }

    bool sendStats(const MeshStatsMsg& s){
        if(!_hasMaster) return false;
        return _tx(_masterMac,&s,sizeof(s));
    }

    bool sendHeartbeat(const MeshHeartbeatMsg& h){
        return _tx(ESPNOW_BROADCAST,&h,sizeof(h));
    }

    bool sendElection(const MeshElectMsg& e){
        return _tx(ESPNOW_BROADCAST,&e,sizeof(e));
    }

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
        Serial.printf("[Mesh] +peer %02X:%02X:%02X:%02X:%02X:%02X\n",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        return true;
    }

    void setMasterMac(const uint8_t* mac){
        memcpy(_masterMac,mac,6); _hasMaster=true; addPeer(mac);
        Serial.printf("[Mesh] master=%02X:%02X:%02X:%02X:%02X:%02X\n",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    }

    bool getMasterMac(uint8_t* out) const {
        if(!_hasMaster) return false; memcpy(out,_masterMac,6); return true;
    }

    bool     hasMaster()    const { return _hasMaster; }
    uint32_t masterLastSeen() const { return _masterLastSeen; }

    // Callbacks
    std::function<void(const MeshJobMsg&)>        onJob;
    std::function<void(const MeshHeartbeatMsg&)>  onHeartbeat;
    std::function<void(const MeshElectMsg&)>      onElect;

private:
    EspNowWorker()=default;

    bool _tx(const uint8_t* mac, const void* data, size_t len){
        if(!_initialized||len>MESH_FRAME_LEN) return false;
        uint8_t frame[MESH_FRAME_LEN]={0}; memcpy(frame,data,len);
        return esp_now_send(mac,frame,MESH_FRAME_LEN)==ESP_OK;
    }

    static void _recv_cb(const esp_now_recv_info_t* info, const uint8_t* data, int len){
        if(len<1||!info) return;
        const uint8_t* mac=info->src_addr;
        EspNowWorker& w=instance();
        uint8_t type=data[0];

        switch(type){
            case MSG_JOB: {
                if(len<(int)sizeof(MeshJobMsg)) return;
                MeshJobMsg j; memcpy(&j,data,sizeof(j));
                // Auto-register sender as master if we don't have one
                if(!w._hasMaster) w.setMasterMac(mac);
                if(w.onJob) w.onJob(j);
                break;
            }
            case MSG_HEARTBEAT: {
                if(len<(int)sizeof(MeshHeartbeatMsg)) return;
                MeshHeartbeatMsg h; memcpy(&h,data,sizeof(h));
                if(h.is_master){
                    if(!w._hasMaster) w.setMasterMac(mac);
                    w._masterLastSeen=millis();
                }
                w.addPeer(mac);
                if(w.onHeartbeat) w.onHeartbeat(h);
                break;
            }
            case MSG_ELECT: {
                if(len<(int)sizeof(MeshElectMsg)) return;
                MeshElectMsg e; memcpy(&e,data,sizeof(e));
                if(w.onElect) w.onElect(e);
                break;
            }
            default: break;
        }
    }

    static void _send_cb(const wifi_tx_info_t*, esp_now_send_status_t){}

    PeerInfo _peers[MESH_MAX_PEERS];
    int      _peerCount      = 0;
    bool     _initialized    = false;
    bool     _hasMaster      = false;
    uint8_t  _masterMac[6]   = {0};
    uint32_t _masterLastSeen = 0;
    uint8_t  _channel        = ESPNOW_CHANNEL;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 7 — MINER  (FreeRTOS task, Core 1)
// ───────────────────────────────────────────────────────────────

struct MinerJob {
    uint8_t  header[80];
    uint8_t  target[32];
    uint32_t nonce_start;
    uint32_t nonce_end;
    uint8_t  job_id;
    bool     valid;
};

struct MinerStats {
    uint32_t hash_rate;       // rolling H/s
    uint32_t total_hashes;    // all time
    uint32_t found_nonces;
    uint32_t last_update_ms;
};

class Miner {
public:
    static Miner& instance(){ static Miner m; return m; }

    void begin(){
        _mutex=xSemaphoreCreateMutex();
        xTaskCreatePinnedToCore(_task,"mining",MINING_TASK_STACK,this,
            MINING_TASK_PRIORITY,nullptr,MINING_TASK_CORE);
        Serial.printf("[Miner] task on core %d\n",MINING_TASK_CORE);
    }

    void setJob(const MinerJob& j){
        if(!_mutex) return;
        xSemaphoreTake(_mutex,portMAX_DELAY);
        _pending=j; _newJob=true;
        xSemaphoreGive(_mutex);
    }

    MinerStats getStats() const { return _stats; }

    std::function<void(const MinerJob&, uint32_t)> onFound;

private:
    Miner()=default;
    static void _task(void* p){ static_cast<Miner*>(p)->_run(); vTaskDelete(nullptr); }

    void _run(){
        uint8_t hash[32];
        uint32_t cnt=0, wstart=millis();

        while(true){
            // Swap in pending job if one arrived
            if(_newJob && _mutex && xSemaphoreTake(_mutex,0)==pdTRUE){
                _job=_pending; _newJob=false; cnt=0; wstart=millis();
                xSemaphoreGive(_mutex);
                Serial.printf("[Miner] job=%d  %08X-%08X\n",
                    _job.job_id,_job.nonce_start,_job.nonce_end);
            }

            if(!_job.valid){ vTaskDelay(pdMS_TO_TICKS(50)); continue; }

            for(uint32_t n=_job.nonce_start; n<_job.nonce_end && !_newJob; n++){
                _job.header[76]= n      & 0xFF;
                _job.header[77]=(n>> 8) & 0xFF;
                _job.header[78]=(n>>16) & 0xFF;
                _job.header[79]=(n>>24) & 0xFF;
                bitcoin_hash(_job.header, hash);
                cnt++;

                if(check_difficulty(hash, _job.target)){
                    _stats.found_nonces++;
                    Serial.printf("[Miner] *** FOUND nonce=%08X ***\n", n);
                    if(onFound) onFound(_job, n);
                }

                if((cnt & 0x3FF)==0){
                    uint32_t el=millis()-wstart;
                    if(el>0) _stats.hash_rate=(cnt*1000UL)/el;
                    _stats.total_hashes+=1024;
                    _stats.last_update_ms=millis();
                    vTaskDelay(1);
                }
            }

            if(!_newJob) _job.valid=false;   // range exhausted — idle
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
//  SECTION 9 — GLOBAL STATE
// ───────────────────────────────────────────────────────────────

static EspNowWorker& mesh  = EspNowWorker::instance();
static Miner&        miner = Miner::instance();

static uint8_t  myMac[6]          = {0};
static uint8_t  currentJobId      = 0;
static uint32_t startupMs         = 0;
static uint32_t lastHeartbeatMs   = 0;
static uint32_t lastStatsMs       = 0;
static uint32_t masterLastSeenMs  = 0;
static uint32_t lastLedMs         = 0;
static bool     ledState          = false;
static bool     electionInProgress= false;
static uint32_t currentNonceStart   = 0;
static uint32_t currentNonceEnd     = 0;
// Cross-core nonce flag (miner=Core1, ESP-NOW=Core0)
static volatile bool     pendingNonceReady = false;
static volatile uint32_t pendingNonce      = 0;
static volatile uint8_t  pendingNonceJob   = 0;

// ───────────────────────────────────────────────────────────────
//  SECTION 10 — ELECTION LOGIC
// ───────────────────────────────────────────────────────────────

// If the master goes silent, workers elect a new one.
// Lowest XOR-of-MAC priority wins. The winner would need WiFi
// credentials to actually connect to the pool — this is a
// best-effort failover.  Set NODE_ROLE ROLE_MASTER on the
// intended backup node and flash MeshMiner32.ino to it instead
// for a fully capable promotion.
static void checkElection(){
    if(electionInProgress) return;
    uint32_t now=millis();
    if(masterLastSeenMs==0){ masterLastSeenMs=now; return; }
    if(now-masterLastSeenMs > (uint32_t)(ELECTION_TRIGGER_COUNT*HEARTBEAT_INTERVAL_MS)){
        electionInProgress=true;
        Serial.println("[Election] master silent — broadcasting candidacy");
        delay(random(0, ELECTION_BACKOFF_MAX_MS));

        MeshElectMsg e={};
        e.msg_type=MSG_ELECT;
        memcpy(e.candidate_mac,myMac,6);
        uint8_t pri=0; for(int i=0;i<6;i++) pri^=myMac[i];
        e.priority=pri;
        mesh.sendElection(e);

        // After back-off: update display to show master is lost
            Serial.println("[Election] candidate broadcast sent");
        // Election resolution happens on whichever node runs the master sketch
    }
}

// ───────────────────────────────────────────────────────────────
//  SECTION 11 — SETUP & LOOP
// ───────────────────────────────────────────────────────────────

void setup(){
    Serial.begin(SERIAL_BAUD);
    delay(100);
    Serial.println("\n=== MeshMiner 32 WORKER ===");

    startupMs=millis();
    WiFi.macAddress(myMac);
    Serial.printf("[Worker] MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
        myMac[0],myMac[1],myMac[2],myMac[3],myMac[4],myMac[5]);


    mesh.begin();

    // ── Job received from master ──────────────────────────────
    mesh.onJob=[](const MeshJobMsg& j){
        Serial.printf("[Worker] job=%d  nonce %08lX-%08lX  chunk=%d\n",
            j.job_id,(unsigned long)j.nonce_start,
            (unsigned long)j.nonce_end, j.assigned_chunk);

        currentJobId      = j.job_id;
        currentNonceStart = j.nonce_start;
        currentNonceEnd   = j.nonce_end;
        electionInProgress= false;   // master is alive

        MinerJob mj={};
        mj.valid      = true;
        mj.job_id     = j.job_id;
        mj.nonce_start= j.nonce_start;
        mj.nonce_end  = j.nonce_end;
        difficulty_to_target(j.pool_difficulty, mj.target);  // pool share diff, NOT nbits
        memcpy(mj.header,     j.version,    4);
        memcpy(mj.header+4,   j.prev_hash,  32);
        memcpy(mj.header+36,  j.merkle_root,32);
        memcpy(mj.header+68,  j.ntime,      4);
        memcpy(mj.header+72,  j.nbits,      4);
        miner.setJob(mj);
    };

    // ── Heartbeat from master ────────────────────────────────
    mesh.onHeartbeat=[](const MeshHeartbeatMsg& hb){
        if(hb.is_master){
            masterLastSeenMs   = millis();
            electionInProgress = false;
        }
    };

    // ── Election message received ────────────────────────────
    mesh.onElect=[](const MeshElectMsg& e){
        // Compare with our own priority; lower wins.
        // A full election protocol would track all candidates —
        // for simplicity we just log and let the master-sketch
        // node handle the actual promotion.
        uint8_t myPri=0; for(int i=0;i<6;i++) myPri^=myMac[i];
        Serial.printf("[Election] candidate %02X:%02X:%02X pri=%d (mine=%d)\n",
            e.candidate_mac[3],e.candidate_mac[4],e.candidate_mac[5],
            e.priority, myPri);
    };

    // ── Nonce found — report to master ──────────────────────
    miner.onFound=[](const MinerJob& job, uint32_t nonce){
        // Do NOT call esp_now_send from Core 1 — flag for Core 0 to handle
        pendingNonce     = nonce;
        pendingNonceJob  = job.job_id;
        pendingNonceReady= true;
    };

    // Blue LED — blinks at hash speed
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    miner.begin();

    // Send first heartbeat immediately so master detects us right away
    // (instead of waiting up to HEARTBEAT_INTERVAL_MS = 5 s)
    {
        MeshHeartbeatMsg hb={};
        hb.msg_type=MSG_HEARTBEAT;
        memcpy(hb.sender_mac,myMac,6);
        hb.is_master=0; hb.uptime_s=0; hb.total_hash_rate=0;
        mesh.sendHeartbeat(hb);
        lastHeartbeatMs=millis();
        Serial.println("[Worker] initial heartbeat sent");
    }

    Serial.println("[Worker] setup complete");
}

void loop(){
    uint32_t now=millis();

    // Handle found nonce safely on Core 0
    if(pendingNonceReady){
        pendingNonceReady=false;
        Serial.printf("[Worker] *** FOUND nonce=%08lX ***\n",(unsigned long)pendingNonce);
        uint8_t masterMac[6];
        if(mesh.getMasterMac(masterMac)){
            MeshResultMsg r={};
            r.msg_type=MSG_RESULT;
            r.job_id  =(uint8_t)pendingNonceJob;
            r.nonce   =pendingNonce;
            memcpy(r.worker_mac,myMac,6);
            mesh.sendResult(r);
        } else {
            Serial.println("[Worker] no master MAC for result");
        }
    }

    // ── Heartbeat broadcast (so master knows we exist) ───────
    if(now-lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS){
        lastHeartbeatMs=now;
        MeshHeartbeatMsg hb={};
        hb.msg_type       = MSG_HEARTBEAT;
        memcpy(hb.sender_mac, myMac, 6);
        hb.is_master      = 0;
        hb.uptime_s       = (now-startupMs)/1000;
        hb.total_hash_rate= miner.getStats().hash_rate;
        mesh.sendHeartbeat(hb);
    }

    // ── Periodic stats report to master ─────────────────────
    if(now-lastStatsMs >= STATS_INTERVAL_MS){
        lastStatsMs=now;
        if(mesh.hasMaster()){
            MinerStats ws=miner.getStats();
            MeshStatsMsg s={};
            s.msg_type     = MSG_STATS;
            memcpy(s.worker_mac, myMac, 6);
            s.hash_rate    = ws.hash_rate;
            s.nonces_tested= ws.total_hashes;
            s.job_id       = currentJobId;
            mesh.sendStats(s);
            Serial.printf("[Worker] %lu H/s  hashes=%lu  found=%lu\n",
                (unsigned long)ws.hash_rate,
                (unsigned long)ws.total_hashes,
                (unsigned long)ws.found_nonces);
        }
        checkElection();
    }


    // Blue LED blinks proportional to hash rate
    {
        uint32_t hr = miner.getStats().hash_rate;
        uint32_t blinkInterval = (hr > 0) ? max(50UL, 1000000UL/hr) : 500;
        if(now - lastLedMs >= blinkInterval){
            lastLedMs = now;
            ledState  = !ledState;
            digitalWrite(LED_PIN, ledState ? HIGH : LOW);
        }
    }

    delay(10);
}
/*
 * ═══════════════════════════════════════════════════════════════
 *  MeshMiner32 Edition  —  WORKER node
 *  Target  : ESP32 DevKit V1  (esp32 board package 3.x)
 *
 *  No WiFi, no pool connection needed on workers.
 *  Workers receive jobs from the master over ESP-NOW,
 *  hash their assigned nonce range, and report results back.
 *
 *  Libraries (install via Arduino Library Manager):
 * *
 *  SPI OLED wiring (same as master):
 *  ┌───────────┬─────────────────────────────┐
 *  │ OLED pin  │ ESP32 pin                   │
 *  ├───────────┼─────────────────────────────┤
 *  │ VCC/3V3   │ 3.3V                        │
 *  │ GND       │ GND                         │
 *  │ SCL       │ GPIO 18  (HW SPI SCK, D18)  │
 *  │ SDA       │ GPIO 23  (HW SPI MOSI, D23) │
 *  │ RES       │ GPIO 4   (D4)               │
 *  │ DC        │ GPIO 2   (D2)               │
 *  │ CS        │ GND on module (no wire)     │
 *  └───────────┴─────────────────────────────┘
 *
 *  Flash this sketch to every ESP32 that acts as a worker.
 *  Flash MeshMiner32.ino to the master (the one with WiFi).
 *  All boards must be on the same ESP-NOW channel (default 1).
 * ═══════════════════════════════════════════════════════════════
 */

// ───────────────────────────────────────────────────────────────
//  SECTION 1 — CONFIGURATION
// ───────────────────────────────────────────────────────────────

// Blue LED pin — blinks at hash speed (GPIO 2 = built-in LED on most DevKit V1)
#define LED_PIN  2

// ESP-NOW channel — must match the WiFi channel your router uses.
// Your master printed "WiFi channel: 11" — so set 11 here.
// If you change routers, check the master Serial output and update this.
#define ESPNOW_CHANNEL  11

// OLED SPI pins (no CS — tied to GND on module)
// SCL -> D18 (GPIO 18, HW SPI SCK)
// SDA -> D23 (GPIO 23, HW SPI MOSI)


// Mining task
#define MINING_TASK_PRIORITY  5
#define MINING_TASK_CORE      1
#define MINING_TASK_STACK     8192

// Mesh timing
#define MESH_MAX_PEERS          20
#define MESH_FRAME_LEN          250
#define PEER_TIMEOUT_MS         15000
#define HEARTBEAT_INTERVAL_MS   5000
#define ELECTION_TRIGGER_COUNT  3
#define ELECTION_BACKOFF_MAX_MS 2000

// Stats report interval (ms)
#define STATS_INTERVAL_MS  5000

#define SERIAL_BAUD  115200

// ───────────────────────────────────────────────────────────────
//  SECTION 2 — INCLUDES
// ───────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <functional>
#include <string.h>

// ───────────────────────────────────────────────────────────────
//  SECTION 3 — FORWARD DECLARATIONS
// ───────────────────────────────────────────────────────────────

struct MinerJob;
struct MinerStats;
struct PeerInfo;
static void checkElection();

// ───────────────────────────────────────────────────────────────
//  SECTION 4 — SHA-256
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
//  SECTION 5 — ESP-NOW MESSAGE STRUCTS
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
//  SECTION 6 — ESP-NOW LAYER  (worker-only: no broadcast job)
// ───────────────────────────────────────────────────────────────

class EspNowWorker {
public:
    static EspNowWorker& instance(){ static EspNowWorker i; return i; }

    // channel: set this to match your router's WiFi channel.
    // The master prints "WiFi channel: N" on Serial — use that number.
    // Default 0 = auto-detect by scanning for the master's beacon.
    bool begin(uint8_t channel=ESPNOW_CHANNEL){
        _channel=channel;
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        // Workers have no WiFi connection, so we set the channel explicitly.
        // It must match whatever channel the master's router is on.
        esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
        Serial.printf("[Mesh] worker ESP-NOW channel: %d\n",(int)_channel);

        if(esp_now_init()!=ESP_OK){ Serial.println("[Mesh] init failed"); return false; }
        esp_now_register_recv_cb(_recv_cb);
        esp_now_register_send_cb(_send_cb);

        // Broadcast peer — receive jobs from any master address
        esp_now_peer_info_t bc={};
        memcpy(bc.peer_addr,ESPNOW_BROADCAST,6);
        bc.channel=_channel; bc.encrypt=false;
        esp_now_add_peer(&bc);

        _initialized=true;
        Serial.println("[Mesh] worker ready");
        return true;
    }

    bool sendResult(const MeshResultMsg& r){
        if(!_hasMaster) return false;
        return _tx(_masterMac,&r,sizeof(r));
    }

    bool sendStats(const MeshStatsMsg& s){
        if(!_hasMaster) return false;
        return _tx(_masterMac,&s,sizeof(s));
    }

    bool sendHeartbeat(const MeshHeartbeatMsg& h){
        return _tx(ESPNOW_BROADCAST,&h,sizeof(h));
    }

    bool sendElection(const MeshElectMsg& e){
        return _tx(ESPNOW_BROADCAST,&e,sizeof(e));
    }

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
        Serial.printf("[Mesh] +peer %02X:%02X:%02X:%02X:%02X:%02X\n",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
        return true;
    }

    void setMasterMac(const uint8_t* mac){
        memcpy(_masterMac,mac,6); _hasMaster=true; addPeer(mac);
        Serial.printf("[Mesh] master=%02X:%02X:%02X:%02X:%02X:%02X\n",
            mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    }

    bool getMasterMac(uint8_t* out) const {
        if(!_hasMaster) return false; memcpy(out,_masterMac,6); return true;
    }

    bool     hasMaster()    const { return _hasMaster; }
    uint32_t masterLastSeen() const { return _masterLastSeen; }

    // Callbacks
    std::function<void(const MeshJobMsg&)>        onJob;
    std::function<void(const MeshHeartbeatMsg&)>  onHeartbeat;
    std::function<void(const MeshElectMsg&)>      onElect;

private:
    EspNowWorker()=default;

    bool _tx(const uint8_t* mac, const void* data, size_t len){
        if(!_initialized||len>MESH_FRAME_LEN) return false;
        uint8_t frame[MESH_FRAME_LEN]={0}; memcpy(frame,data,len);
        return esp_now_send(mac,frame,MESH_FRAME_LEN)==ESP_OK;
    }

    static void _recv_cb(const esp_now_recv_info_t* info, const uint8_t* data, int len){
        if(len<1||!info) return;
        const uint8_t* mac=info->src_addr;
        EspNowWorker& w=instance();
        uint8_t type=data[0];

        switch(type){
            case MSG_JOB: {
                if(len<(int)sizeof(MeshJobMsg)) return;
                MeshJobMsg j; memcpy(&j,data,sizeof(j));
                // Auto-register sender as master if we don't have one
                if(!w._hasMaster) w.setMasterMac(mac);
                if(w.onJob) w.onJob(j);
                break;
            }
            case MSG_HEARTBEAT: {
                if(len<(int)sizeof(MeshHeartbeatMsg)) return;
                MeshHeartbeatMsg h; memcpy(&h,data,sizeof(h));
                if(h.is_master){
                    if(!w._hasMaster) w.setMasterMac(mac);
                    w._masterLastSeen=millis();
                }
                w.addPeer(mac);
                if(w.onHeartbeat) w.onHeartbeat(h);
                break;
            }
            case MSG_ELECT: {
                if(len<(int)sizeof(MeshElectMsg)) return;
                MeshElectMsg e; memcpy(&e,data,sizeof(e));
                if(w.onElect) w.onElect(e);
                break;
            }
            default: break;
        }
    }

    static void _send_cb(const wifi_tx_info_t*, esp_now_send_status_t){}

    PeerInfo _peers[MESH_MAX_PEERS];
    int      _peerCount      = 0;
    bool     _initialized    = false;
    bool     _hasMaster      = false;
    uint8_t  _masterMac[6]   = {0};
    uint32_t _masterLastSeen = 0;
    uint8_t  _channel        = ESPNOW_CHANNEL;
};

// ───────────────────────────────────────────────────────────────
//  SECTION 7 — MINER  (FreeRTOS task, Core 1)
// ───────────────────────────────────────────────────────────────

struct MinerJob {
    uint8_t  header[80];
    uint8_t  target[32];
    uint32_t nonce_start;
    uint32_t nonce_end;
    uint8_t  job_id;
    bool     valid;
};

struct MinerStats {
    uint32_t hash_rate;       // rolling H/s
    uint32_t total_hashes;    // all time
    uint32_t found_nonces;
    uint32_t last_update_ms;
};

class Miner {
public:
    static Miner& instance(){ static Miner m; return m; }

    void begin(){
        _mutex=xSemaphoreCreateMutex();
        xTaskCreatePinnedToCore(_task,"mining",MINING_TASK_STACK,this,
            MINING_TASK_PRIORITY,nullptr,MINING_TASK_CORE);
        Serial.printf("[Miner] task on core %d\n",MINING_TASK_CORE);
    }

    void setJob(const MinerJob& j){
        if(!_mutex) return;
        xSemaphoreTake(_mutex,portMAX_DELAY);
        _pending=j; _newJob=true;
        xSemaphoreGive(_mutex);
    }

    MinerStats getStats() const { return _stats; }

    std::function<void(const MinerJob&, uint32_t)> onFound;

private:
    Miner()=default;
    static void _task(void* p){ static_cast<Miner*>(p)->_run(); vTaskDelete(nullptr); }

    void _run(){
        uint8_t hash[32];
        uint32_t cnt=0, wstart=millis();

        while(true){
            // Swap in pending job if one arrived
            if(_newJob && _mutex && xSemaphoreTake(_mutex,0)==pdTRUE){
                _job=_pending; _newJob=false; cnt=0; wstart=millis();
                xSemaphoreGive(_mutex);
                Serial.printf("[Miner] job=%d  %08X-%08X\n",
                    _job.job_id,_job.nonce_start,_job.nonce_end);
            }

            if(!_job.valid){ vTaskDelay(pdMS_TO_TICKS(50)); continue; }

            for(uint32_t n=_job.nonce_start; n<_job.nonce_end && !_newJob; n++){
                _job.header[76]= n      & 0xFF;
                _job.header[77]=(n>> 8) & 0xFF;
                _job.header[78]=(n>>16) & 0xFF;
                _job.header[79]=(n>>24) & 0xFF;
                bitcoin_hash(_job.header, hash);
                cnt++;

                if(check_difficulty(hash, _job.target)){
                    _stats.found_nonces++;
                    Serial.printf("[Miner] *** FOUND nonce=%08X ***\n", n);
                    if(onFound) onFound(_job, n);
                }

                if((cnt & 0x3FF)==0){
                    uint32_t el=millis()-wstart;
                    if(el>0) _stats.hash_rate=(cnt*1000UL)/el;
                    _stats.total_hashes+=1024;
                    _stats.last_update_ms=millis();
                    vTaskDelay(1);
                }
            }

            if(!_newJob) _job.valid=false;   // range exhausted — idle
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
//  SECTION 9 — GLOBAL STATE
// ───────────────────────────────────────────────────────────────

static EspNowWorker& mesh  = EspNowWorker::instance();
static Miner&        miner = Miner::instance();

static uint8_t  myMac[6]          = {0};
static uint8_t  currentJobId      = 0;
static uint32_t startupMs         = 0;
static uint32_t lastHeartbeatMs   = 0;
static uint32_t lastStatsMs       = 0;
static uint32_t masterLastSeenMs  = 0;
static uint32_t lastLedMs         = 0;
static bool     ledState          = false;
static bool     electionInProgress= false;
static uint32_t currentNonceStart   = 0;
static uint32_t currentNonceEnd     = 0;
// Cross-core nonce flag (miner=Core1, ESP-NOW=Core0)
static volatile bool     pendingNonceReady = false;
static volatile uint32_t pendingNonce      = 0;
static volatile uint8_t  pendingNonceJob   = 0;

// ───────────────────────────────────────────────────────────────
//  SECTION 10 — ELECTION LOGIC
// ───────────────────────────────────────────────────────────────

// If the master goes silent, workers elect a new one.
// Lowest XOR-of-MAC priority wins. The winner would need WiFi
// credentials to actually connect to the pool — this is a
// best-effort failover.  Set NODE_ROLE ROLE_MASTER on the
// intended backup node and flash MeshMiner32.ino to it instead
// for a fully capable promotion.
static void checkElection(){
    if(electionInProgress) return;
    uint32_t now=millis();
    if(masterLastSeenMs==0){ masterLastSeenMs=now; return; }
    if(now-masterLastSeenMs > (uint32_t)(ELECTION_TRIGGER_COUNT*HEARTBEAT_INTERVAL_MS)){
        electionInProgress=true;
        Serial.println("[Election] master silent — broadcasting candidacy");
        delay(random(0, ELECTION_BACKOFF_MAX_MS));

        MeshElectMsg e={};
        e.msg_type=MSG_ELECT;
        memcpy(e.candidate_mac,myMac,6);
        uint8_t pri=0; for(int i=0;i<6;i++) pri^=myMac[i];
        e.priority=pri;
        mesh.sendElection(e);

        // After back-off: update display to show master is lost
            Serial.println("[Election] candidate broadcast sent");
        // Election resolution happens on whichever node runs the master sketch
    }
}

// ───────────────────────────────────────────────────────────────
//  SECTION 11 — SETUP & LOOP
// ───────────────────────────────────────────────────────────────

void setup(){
    Serial.begin(SERIAL_BAUD);
    delay(100);
    Serial.println("\n=== MeshMiner 32 WORKER ===");

    startupMs=millis();
    WiFi.macAddress(myMac);
    Serial.printf("[Worker] MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
        myMac[0],myMac[1],myMac[2],myMac[3],myMac[4],myMac[5]);


    mesh.begin();

    // ── Job received from master ──────────────────────────────
    mesh.onJob=[](const MeshJobMsg& j){
        Serial.printf("[Worker] job=%d  nonce %08lX-%08lX  chunk=%d\n",
            j.job_id,(unsigned long)j.nonce_start,
            (unsigned long)j.nonce_end, j.assigned_chunk);

        currentJobId      = j.job_id;
        currentNonceStart = j.nonce_start;
        currentNonceEnd   = j.nonce_end;
        electionInProgress= false;   // master is alive

        MinerJob mj={};
        mj.valid      = true;
        mj.job_id     = j.job_id;
        mj.nonce_start= j.nonce_start;
        mj.nonce_end  = j.nonce_end;
        nbits_to_target(j.nbits, mj.target);
        memcpy(mj.header,     j.version,    4);
        memcpy(mj.header+4,   j.prev_hash,  32);
        memcpy(mj.header+36,  j.merkle_root,32);
        memcpy(mj.header+68,  j.ntime,      4);
        memcpy(mj.header+72,  j.nbits,      4);
        miner.setJob(mj);
    };

    // ── Heartbeat from master ────────────────────────────────
    mesh.onHeartbeat=[](const MeshHeartbeatMsg& hb){
        if(hb.is_master){
            masterLastSeenMs   = millis();
            electionInProgress = false;
        }
    };

    // ── Election message received ────────────────────────────
    mesh.onElect=[](const MeshElectMsg& e){
        // Compare with our own priority; lower wins.
        // A full election protocol would track all candidates —
        // for simplicity we just log and let the master-sketch
        // node handle the actual promotion.
        uint8_t myPri=0; for(int i=0;i<6;i++) myPri^=myMac[i];
        Serial.printf("[Election] candidate %02X:%02X:%02X pri=%d (mine=%d)\n",
            e.candidate_mac[3],e.candidate_mac[4],e.candidate_mac[5],
            e.priority, myPri);
    };

    // ── Nonce found — report to master ──────────────────────
    miner.onFound=[](const MinerJob& job, uint32_t nonce){
        // Do NOT call esp_now_send from Core 1 — flag for Core 0 to handle
        pendingNonce     = nonce;
        pendingNonceJob  = job.job_id;
        pendingNonceReady= true;
    };

    // Blue LED — blinks at hash speed
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    miner.begin();

    // Send first heartbeat immediately so master detects us right away
    // (instead of waiting up to HEARTBEAT_INTERVAL_MS = 5 s)
    {
        MeshHeartbeatMsg hb={};
        hb.msg_type=MSG_HEARTBEAT;
        memcpy(hb.sender_mac,myMac,6);
        hb.is_master=0; hb.uptime_s=0; hb.total_hash_rate=0;
        mesh.sendHeartbeat(hb);
        lastHeartbeatMs=millis();
        Serial.println("[Worker] initial heartbeat sent");
    }

    Serial.println("[Worker] setup complete");
}

void loop(){
    uint32_t now=millis();

    // Handle found nonce safely on Core 0
    if(pendingNonceReady){
        pendingNonceReady=false;
        Serial.printf("[Worker] *** FOUND nonce=%08lX ***\n",(unsigned long)pendingNonce);
        uint8_t masterMac[6];
        if(mesh.getMasterMac(masterMac)){
            MeshResultMsg r={};
            r.msg_type=MSG_RESULT;
            r.job_id  =(uint8_t)pendingNonceJob;
            r.nonce   =pendingNonce;
            memcpy(r.worker_mac,myMac,6);
            mesh.sendResult(r);
        } else {
            Serial.println("[Worker] no master MAC for result");
        }
    }

    // ── Heartbeat broadcast (so master knows we exist) ───────
    if(now-lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS){
        lastHeartbeatMs=now;
        MeshHeartbeatMsg hb={};
        hb.msg_type       = MSG_HEARTBEAT;
        memcpy(hb.sender_mac, myMac, 6);
        hb.is_master      = 0;
        hb.uptime_s       = (now-startupMs)/1000;
        hb.total_hash_rate= miner.getStats().hash_rate;
        mesh.sendHeartbeat(hb);
    }

    // ── Periodic stats report to master ─────────────────────
    if(now-lastStatsMs >= STATS_INTERVAL_MS){
        lastStatsMs=now;
        if(mesh.hasMaster()){
            MinerStats ws=miner.getStats();
            MeshStatsMsg s={};
            s.msg_type     = MSG_STATS;
            memcpy(s.worker_mac, myMac, 6);
            s.hash_rate    = ws.hash_rate;
            s.nonces_tested= ws.total_hashes;
            s.job_id       = currentJobId;
            mesh.sendStats(s);
            Serial.printf("[Worker] %lu H/s  hashes=%lu  found=%lu\n",
                (unsigned long)ws.hash_rate,
                (unsigned long)ws.total_hashes,
                (unsigned long)ws.found_nonces);
        }
        checkElection();
    }


    // Blue LED blinks proportional to hash rate
    {
        uint32_t hr = miner.getStats().hash_rate;
        uint32_t blinkInterval = (hr > 0) ? max(50UL, 1000000UL/hr) : 500;
        if(now - lastLedMs >= blinkInterval){
            lastLedMs = now;
            ledState  = !ledState;
            digitalWrite(LED_PIN, ledState ? HIGH : LOW);
        }
    }

    delay(10);
}
